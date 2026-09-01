#include "engine.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

#include "adapter.h"
#include "atomic_file.h"
#include "contracts.h"
#include "json_io.h"
#include "run_store.h"
#include "subprocess.h"

namespace fs = std::filesystem;

// what _execute_stage returns in python: the handoff plus the metadata recorded
// alongside it in the event log.
struct stage_outcome
{
	json result;
	json metadata;
};

static fs::path engine_path_from_utf8(const std::string &value)
{
	std::u8string text;
	text.reserve(value.size());
	for (const unsigned char byte : value) text.push_back(static_cast<char8_t>(byte));
	return fs::path(text);
}

bool engine_is_terminal(const std::string &status)
{
	return status == "complete" || status == "blocked" || status == "failed";
}

void engine_configure(engine &e,
					  const project_config &config,
					  definitions &defs,
					  sidecar &service)
{
	e.config = config;
	e.defs = &defs;
	e.service = &service;
}

// ------------------------------------------------------------------ time

// python: dt.datetime.now(dt.UTC).isoformat(timespec="seconds")
static std::string engine_utc_now()
{
	const std::time_t now = std::time(nullptr);
	std::tm parts = {};
#ifdef _WIN32
	gmtime_s(&parts, &now);
#else
	gmtime_r(&now, &parts);
#endif
	char buffer[40];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S+00:00", &parts);
	return buffer;
}

// python: dt.datetime.now().strftime('%Y%m%d-%H%M%S')
//
// note this is LOCAL time while event timestamps are UTC. the inconsistency is
// in harness/engine.py and is reproduced deliberately rather than quietly fixed,
// because run ids are directory names that already exist on disk.
static std::string engine_local_stamp()
{
	const std::time_t now = std::time(nullptr);
	std::tm parts = {};
#ifdef _WIN32
	localtime_s(&parts, &now);
#else
	localtime_r(&now, &parts);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y%m%d-%H%M%S", &parts);
	return buffer;
}

// python: uuid.uuid4().hex[:6]
static std::string engine_run_suffix()
{
	static thread_local std::mt19937 source{std::random_device{}()};
	std::uniform_int_distribution<int> nibble(0, 15);

	std::string out;
	for (int i = 0; i < 6; ++i) out.push_back("0123456789abcdef"[nibble(source)]);
	return out;
}

static std::string engine_execution_id()
{
	static thread_local std::mt19937 source = [] {
		std::random_device random;
		std::uint32_t seeds[8];
		for (std::uint32_t &seed : seeds) seed = random();
		std::seed_seq sequence(std::begin(seeds), std::end(seeds));
		return std::mt19937(sequence);
	}();
	std::uniform_int_distribution<int> nibble(0, 15);
	std::string out;
	out.reserve(32);
	for (int i = 0; i < 32; ++i) out.push_back("0123456789abcdef"[nibble(source)]);
	return out;
}

// --------------------------------------------------------------- helpers

// harness/engine.py uses str.format, which also gives meaning to a stray brace
// in a configured gate command. plain substitution is used instead, matching
// what adapters/cli_agent.py already does.
static std::string engine_expand(const std::string &part, const json &context)
{
	const json &task = context.value("task", json::object());

	const std::pair<const char *, std::string> tokens[] = {
	  {"{task_file}", context.value("task_file", std::string{})},
	  {"{run_dir}", context.value("run_dir", std::string{})},
	  {"{task_id}", task.is_object() ? task.value("id", std::string{}) : std::string{}},
	  {"{kind}", task.is_object() ? task.value("kind", std::string{}) : std::string{}},
	};

	std::string out = part;
	for (const auto &[token, value] : tokens)
	{
		const std::string needle = token;
		std::size_t at = 0;
		for (;;)
		{
			const std::size_t found = out.find(needle, at);
			if (found == std::string::npos)
				break;
			out.replace(found, needle.size(), value);
			at = found + value.size();
		}
	}
	return out;
}

// a gate is any project command. nonzero exit means the work needs revision, not
// that the harness broke, so this never sets out_error.
static json engine_run_gate(const std::string &name,
							const command_spec &spec,
							const project_config &config,
							const json &context)
{
	subprocess_options options;
	options.command.reserve(spec.command.size());
	for (const std::string &part : spec.command)
	{
		options.command.push_back(engine_expand(part, context));
	}
	options.working_directory = config.root;
	options.environment = config.environment;
	for (const auto &[key, value] : spec.environment) options.environment[key] = value;
	options.inherit_environment = spec.inherit_environment;
	options.merge_stderr = true;
	options.timeout_seconds = spec.timeout_seconds;

	json gate;
	gate["command"] = options.command;

	odin_error run_error;
	const subprocess_result completed = subprocess_run(options, run_error);
	if (failed(run_error))
	{
		gate["status"] = "failed";
		gate["summary"] = "gate '" + name + "' could not run: " + run_error.message;
		gate["exit_code"] = nullptr;
		gate["output"] = "";
		return gate;
	}

	gate["status"] = completed.exit_code == 0 ? "passed" : "failed";
	gate["summary"] = "gate '" + name + "' exited " + std::to_string(completed.exit_code);
	gate["exit_code"] = completed.exit_code;
	gate["output"] = completed.stdout_text;
	return gate;
}

// ---------------------------------------------------------------- stages

static bool engine_run_agent(engine &e,
							 const json &stage,
							 const json &context,
							 const std::string &model_override,
							 stage_outcome &out_outcome,
							 odin_error &out_error)
{
	const json *agent =
	  definitions_load_agent(*e.defs, stage.at("agent").get<std::string>(), out_error);
	if (agent == nullptr)
		return false;

	json skills = json::array();
	for (const json &identifier : agent->value("skills", json::array()))
	{
		const json *skill =
		  definitions_load_skill(*e.defs, identifier.get<std::string>(), out_error);
		if (skill == nullptr)
			return false;
		skills.push_back(*skill);
	}

	const model_profile *profile =
	  config_model_for(e.config, agent->at("id").get<std::string>(), model_override, out_error);
	if (profile == nullptr)
		return false;

	const command_spec *adapter = config_adapter_for(e.config, *profile, out_error);
	if (adapter == nullptr)
		return false;

	// the routing table is the stage's business, not the agent's
	json visible = json::object();
	for (auto entry = stage.begin(); entry != stage.end(); ++entry)
	{
		if (entry.key() != "on")
			visible[entry.key()] = entry.value();
	}

	json request;
	request["contract"] = "handoff/v1";
	request["agent"] = *agent;
	request["skills"] = skills;
	request["stage"] = visible;
	request["task"] = context.at("task");
	request["artifacts"] = context.at("artifacts");
	request["required_output"] = json{{"status", "approved | revision | blocked"},
									  {"summary", "string"},
									  {"artifacts", "object"},
									  {"findings", "string[]"}};

	odin_error adapter_error;
	const adapter_result produced =
	  adapter_run(*adapter, *profile, request, e.config, adapter_error);

	if (failed(adapter_error))
	{
		// an adapter that fails is a blocked stage, not a broken harness: the
		// run stays durable and the reason is recorded in the handoff.
		out_outcome.result = json{{"status", "blocked"},
								  {"summary", adapter_error.message},
								  {"artifacts", json::object()},
								  {"findings", json::array({adapter_error.message})}};
		out_outcome.metadata =
		  json{{"adapter", profile->adapter}, {"model_profile", profile->name}};
		return true;
	}

	out_outcome.result = produced.response;
	out_outcome.metadata = produced.metadata;
	return true;
}

static bool engine_run_staging(engine &e,
							   const json &stage,
							   const json &context,
							   stage_outcome &out_outcome)
{
	const std::string source = stage.value("paths_from", std::string{"implementation"});
	const json &artifacts = context.at("artifacts");

	json candidates = json::array();
	if (artifacts.contains(source) && artifacts.at(source).is_object())
	{
		const json &produced = artifacts.at(source);
		if (produced.contains("artifacts") && produced.at("artifacts").is_object())
		{
			const json &inner = produced.at("artifacts");
			if (inner.contains("changed_files"))
				candidates = inner.at("changed_files");
		}
	}

	if (!candidates.is_array() || candidates.empty())
	{
		out_outcome.result =
		  json{{"status", "blocked"},
			   {"summary", "no explicit changed_files artifact was supplied for staging"},
			   {"artifacts", json::object()},
			   {"findings",
				json::array(
				  {"explicit staging requires a non-empty changed_files array"})}};
		out_outcome.metadata = json{{"operation", "git-stage"}};
		return true;
	}

	json validated = json::array();
	json invalid = json::array();
	std::vector<std::string> seen;
	std::error_code root_error;
	const fs::path canonical_root = fs::weakly_canonical(e.config.root, root_error);
	for (std::size_t index = 0; index < candidates.size(); ++index)
	{
		const json &candidate = candidates.at(index);
		const std::string label = "changed_files[" + std::to_string(index) + "]";
		if (!candidate.is_string())
		{
			invalid.push_back(label + " must be a string");
			continue;
		}
		const std::string value = candidate.get<std::string>();
		if (value.empty())
		{
			invalid.push_back(label + " must not be empty");
			continue;
		}
		if (value.find('\0') != std::string::npos || value.find('\\') != std::string::npos)
		{
			invalid.push_back(label + " must be a portable project-relative path");
			continue;
		}
		const fs::path relative = engine_path_from_utf8(value);
		bool normal = !relative.is_absolute() && !relative.has_root_name() &&
					  !relative.has_root_directory() && value != "." && relative.generic_string() == value;
		for (const fs::path &part : relative)
		{
			if (part == "." || part == "..")
				normal = false;
		}
		if (!normal)
		{
			invalid.push_back(label + " must be a normalized project-relative path");
			continue;
		}
		const auto duplicate = std::find(seen.begin(), seen.end(), value);
		if (duplicate != seen.end())
		{
			invalid.push_back(label + " duplicates an earlier path");
			continue;
		}
		seen.push_back(value);

		std::error_code path_error;
		const fs::path resolved = fs::weakly_canonical(e.config.root / relative, path_error);
		const fs::path within = resolved.lexically_relative(canonical_root);
		if (root_error || path_error || within.empty() || *within.begin() == "..")
		{
			invalid.push_back(label + " resolves outside the project root");
			continue;
		}
		validated.push_back(value);
	}
	if (!invalid.empty())
	{
		out_outcome.result = json{{"status", "blocked"},
								  {"summary", "explicit changed_files artifact is invalid"},
								  {"artifacts", json{{"staging_manifest", candidates}}},
								  {"findings", invalid}};
		out_outcome.metadata = json{{"operation", "git-stage"}, {"executed", false}};
		return true;
	}

	if (!e.config.stage_on_success)
	{
		out_outcome.result = json{
		  {"status", "approved"},
		  {"summary", "explicit staging manifest prepared; automatic staging is disabled"},
		  {"artifacts",
		   json{{"staged_files", json::array()}, {"staging_manifest", validated}}},
		  {"findings", json::array()}};
		out_outcome.metadata =
		  json{{"operation", "git-stage"}, {"executed", false}};
		return true;
	}

	subprocess_options root_options;
	root_options.command = {"git", "rev-parse", "--show-toplevel"};
	root_options.working_directory = e.config.root;
	root_options.merge_stderr = true;
	root_options.timeout_seconds = e.config.git_timeout_seconds;
	odin_error root_run_error;
	const subprocess_result root_result = subprocess_run(root_options, root_run_error);
	std::string reported_root = root_result.stdout_text;
	while (!reported_root.empty() && std::isspace(static_cast<unsigned char>(reported_root.back())))
		reported_root.pop_back();
	std::error_code equivalent_error;
	const bool same_root = !failed(root_run_error) && root_result.exit_code == 0 &&
						   fs::equivalent(e.config.root, engine_path_from_utf8(reported_root), equivalent_error) &&
						   !equivalent_error;
	if (!same_root)
	{
		const std::string finding = failed(root_run_error) ? root_run_error.message : root_result.exit_code != 0 ? reported_root
																												 : "project root must be the Git worktree root";
		out_outcome.result = json{{"status", "blocked"},
								  {"summary", "explicit git staging requires the project root to be a Git worktree"},
								  {"artifacts", json{{"staged_files", json::array()},
													 {"staging_manifest", validated}}},
								  {"findings", json::array({finding})}};
		out_outcome.metadata = json{{"operation", "git-stage"}, {"executed", false}};
		return true;
	}

	json target_findings = json::array();
	for (std::size_t index = 0; index < validated.size(); ++index)
	{
		const fs::path target =
		  e.config.root / engine_path_from_utf8(validated.at(index).get<std::string>());
		std::error_code status_error;
		if (fs::is_directory(target, status_error))
			target_findings.push_back("changed_files[" + std::to_string(index) + "] names a directory");
	}
	if (!target_findings.empty())
	{
		out_outcome.result = json{{"status", "blocked"},
								  {"summary", "explicit changed_files artifact is invalid"},
								  {"artifacts", json{{"staged_files", json::array()},
													 {"staging_manifest", validated}}},
								  {"findings", target_findings}};
		out_outcome.metadata = json{{"operation", "git-stage"}, {"executed", false}};
		return true;
	}

	subprocess_options options;
	options.command = {"git", "--literal-pathspecs", "add", "--"};
	for (const json &path : validated) options.command.push_back(path.get<std::string>());
	options.working_directory = e.config.root;
	options.merge_stderr = true;
	options.timeout_seconds = e.config.git_timeout_seconds;

	odin_error run_error;
	const subprocess_result completed = subprocess_run(options, run_error);
	const int exit_code = failed(run_error) ? 1 : completed.exit_code;
	const std::string output = failed(run_error) ? run_error.message : completed.stdout_text;

	out_outcome.result =
	  json{{"status", exit_code == 0 ? "approved" : "blocked"},
		   {"summary", "explicit git staging exited " + std::to_string(exit_code)},
		   {"artifacts", json{{"staged_files", exit_code == 0 ? validated : json::array()},
							  {"staging_manifest", validated},
							  {"output", output}}},
		   {"findings", json::array()}};

	if (exit_code != 0)
	{
		std::string trimmed = output;
		const std::size_t first = trimmed.find_first_not_of(" \t\n\r\f\v");
		const std::size_t last = trimmed.find_last_not_of(" \t\n\r\f\v");
		trimmed = first == std::string::npos ? "" : trimmed.substr(first, last - first + 1);
		out_outcome.result["findings"] = json::array({trimmed});
	}
	out_outcome.metadata = json{{"operation", "git-stage"},
								{"exit_code", failed(run_error) ? json(nullptr) : json(exit_code)},
								{"executed", true}};
	return true;
}

static bool engine_execute_stage(engine &e,
								 const json &stage,
								 const json &context,
								 const std::string &model_override,
								 stage_outcome &out_outcome,
								 odin_error &out_error)
{
	const std::string kind = stage.value("kind", std::string{});

	if (kind == "agent")
	{
		return engine_run_agent(e, stage, context, model_override, out_outcome, out_error);
	}

	if (kind == "gate")
	{
		const std::string name = stage.at("gate").get<std::string>();
		const auto configured = e.config.gates.find(name);
		if (configured == e.config.gates.end())
		{
			fail(out_error, error_kind::workflow, "gate '" + name + "' is not configured");
			return false;
		}

		const json gate = engine_run_gate(name, configured->second, e.config, context);
		const bool approved = gate.at("status") == "passed";
		const std::string summary = gate.at("summary").get<std::string>();

		out_outcome.result = json{{"status", approved ? "approved" : "revision"},
								  {"summary", summary},
								  {"artifacts", json{{"gate", gate}}},
								  {"findings",
								   approved ? json::array() : json::array({summary})}};
		out_outcome.metadata = json{{"gate", name}};
		return true;
	}

	if (kind == "checkpoint")
	{
		const json required = stage.value("requires", json::array());
		const json &artifacts = context.at("artifacts");

		json missing = json::array();
		for (const json &name : required)
		{
			if (!artifacts.contains(name.get<std::string>()))
				missing.push_back(name);
		}

		const std::string id = stage.at("id").get<std::string>();
		if (!missing.empty())
		{
			json findings = json::array();
			for (const json &name : missing)
			{
				findings.push_back("missing artifact: " + name.get<std::string>());
			}
			out_outcome.result = json{{"status", "blocked"},
									  {"summary", "checkpoint is missing required artifacts"},
									  {"artifacts", json{{"missing", missing}}},
									  {"findings", findings}};
		}
		else
		{
			out_outcome.result = json{{"status", "approved"},
									  {"summary", "checkpoint '" + id + "' satisfied"},
									  {"artifacts", json{{"required", required}}},
									  {"findings", json::array()}};
		}
		out_outcome.metadata = json{{"checkpoint", id}};
		return true;
	}

	if (kind == "stage")
	{
		return engine_run_staging(e, stage, context, out_outcome);
	}

	fail(out_error, error_kind::workflow, "unsupported stage kind: " + kind);
	return false;
}

// ------------------------------------------------------------------- run

fs::path engine_create_run(engine &e,
						   const json &task,
						   const fs::path &task_file,
						   odin_error &out_error)
{
	contract_validate(*e.service, task, "task", file_path_utf8(task_file), out_error);
	if (failed(out_error))
		return {};

	const json *workflow =
	  definitions_load_workflow(*e.defs, task.at("kind").get<std::string>(), out_error);
	if (workflow == nullptr)
		return {};

	std::string run_id;
	fs::path run_dir;
	std::error_code create_error;
	for (int attempt = 0; attempt < 10; ++attempt)
	{
		run_id = engine_local_stamp() + "-" + task.at("id").get<std::string>() + "-" +
				 engine_run_suffix();
		run_dir = e.config.state_dir / run_id;
		const fs::path candidate_relative = run_dir.lexically_relative(e.config.root);
		if (candidate_relative.empty() || *candidate_relative.begin() == "..")
		{
			fail(out_error, error_kind::workflow,
				 "'" + file_path_utf8(run_dir) + "' is not in the subpath of '" +
				   file_path_utf8(e.config.root) + "'");
			return {};
		}
		if (fs::create_directories(run_dir, create_error))
			break;
		if (create_error)
		{
			fail(out_error, error_kind::io,
				 "could not create " + file_path_utf8(run_dir) + " (" + create_error.message() + ")");
			return {};
		}
		run_dir.clear();
	}
	if (run_dir.empty())
	{
		fail(out_error, error_kind::io, "could not allocate a unique run directory");
		return {};
	}

	fs::path relative = run_dir.lexically_relative(e.config.root);
	if (relative.empty() || *relative.begin() == "..")
	{
		fail(out_error, error_kind::workflow,
			 "'" + file_path_utf8(run_dir) + "' is not in the subpath of '" +
			   file_path_utf8(e.config.root) + "'");
		return {};
	}
	relative.make_preferred();

	std::error_code code;
	fs::path absolute_task = fs::weakly_canonical(task_file, code);
	if (code)
		absolute_task = fs::absolute(task_file);

	json context;
	context["run_id"] = run_id;
	context["run_dir"] = file_path_utf8(relative);
	context["task_file"] = file_path_utf8(absolute_task);
	context["task"] = task;
	context["artifacts"] = json::object();
	context["history"] = json::array();

	const std::string now = engine_utc_now();
	json state;
	state["schema_version"] = 2;
	state["run_id"] = run_id;
	state["workflow"] = workflow->at("id");
	state["status"] = "running";
	state["current_stage"] = workflow->at("start");
	state["transitions"] = 0;
	state["stage_attempts"] = json::object();
	state["created_at"] = now;
	state["updated_at"] = now;

	json_write_atomic(run_dir / "task.json", task, out_error);
	if (failed(out_error))
		return {};
	json_write_atomic(run_dir / "context.json", context, out_error);
	if (failed(out_error))
		return {};
	json_write_atomic(run_dir / "state.json", state, out_error);
	if (failed(out_error))
		return {};

	return run_dir;
}

static json engine_completion_record(const json &journal)
{
	return json{{"sequence", journal.at("sequence")},
				{"stage", journal.at("stage")},
				{"kind", journal.at("kind")},
				{"attempt", journal.at("attempt")},
				{"at", journal.at("at")},
				{"result", journal.at("result")},
				{"metadata", journal.at("metadata")}};
}

static bool engine_publish_event(const fs::path &run_dir,
								 const json &record,
								 odin_error &out_error)
{
	char name[32];
	std::snprintf(name, sizeof(name), "%03d-", record.at("sequence").get<int>());
	const fs::path path = run_dir / "events" /
						  (std::string(name) + record.at("stage").get<std::string>() + ".json");
	const file_publish_result result = json_write_create_only(path, record, out_error);
	if (result == file_publish_result::created)
		return true;
	if (result == file_publish_result::already_exists)
	{
		odin_error read_error;
		const json existing = json_read(path, read_error);
		if (!failed(read_error) && existing == record)
			return true;
		fail(out_error, error_kind::workflow,
			 "immutable event conflicts with " + file_path_utf8(path));
	}
	return false;
}

static bool engine_apply_completion(const fs::path &run_dir,
									const json &stage,
									const json &completed,
									json &context,
									json &state,
									odin_error &out_error)
{
	const json record = engine_completion_record(completed);
	const bool state_applied = state.value("last_completed_execution_id", std::string{}) ==
							   completed.at("execution_id").get<std::string>();
	if (!engine_publish_event(run_dir, record, out_error))
		return false;

	const int sequence = record.at("sequence").get<int>();
	json &history = context["history"];
	if (static_cast<int>(history.size()) < sequence)
	{
		if (static_cast<int>(history.size()) + 1 != sequence)
		{
			fail(out_error, error_kind::workflow, "run history has a sequence gap");
			return false;
		}
		history.push_back(record);
		context["artifacts"][stage.value("output", record.at("stage").get<std::string>())] =
		  record.at("result");
		json_write_atomic(run_dir / "context.json", context, out_error);
		if (failed(out_error))
			return false;
	}
	else if (history.at(static_cast<std::size_t>(sequence - 1)) != record)
	{
		fail(out_error, error_kind::workflow,
			 "run history conflicts at sequence " + std::to_string(sequence));
		return false;
	}

	if (!state_applied)
	{
		const std::string outcome_status = record.at("result").at("status").get<std::string>();
		const std::string next =
		  stage.value("on", json::object()).value(outcome_status, std::string{});
		if (next.empty())
		{
			state["status"] = "blocked";
			state["reason"] = "stage '" + record.at("stage").get<std::string>() +
							  "' has no transition for '" + outcome_status + "'";
		}
		else
		{
			state["transitions"] = state.at("transitions").get<int>() + 1;
			if (engine_is_terminal(next))
			{
				state["status"] = next;
				state["current_stage"] = record.at("stage");
			}
			else
			{
				state["status"] = "running";
				state["current_stage"] = next;
			}
		}
	}
	state["schema_version"] = 2;
	state["last_completed_execution_id"] = completed.at("execution_id");
	state.erase("in_progress");
	state.erase("reason_code");
	state["updated_at"] = engine_utc_now();
	json_write_atomic(run_dir / "state.json", state, out_error);
	return !failed(out_error);
}

static const json *engine_find_completion(const std::vector<json> &journal,
										  const std::string &execution_id)
{
	for (const json &record : journal)
	{
		if (record.value("type", std::string{}) == "stage_completed" &&
			record.value("execution_id", std::string{}) == execution_id)
			return &record;
	}
	return nullptr;
}

static const json *engine_find_next_completion(const std::vector<json> &journal,
											   const json &state,
											   const json &context,
											   odin_error &out_error)
{
	const int next_sequence = static_cast<int>(context.at("history").size()) + 1;
	const std::string current_stage = state.at("current_stage").get<std::string>();
	const json *found = nullptr;
	for (const json &record : journal)
	{
		if (record.at("type") != "stage_completed")
			continue;
		const int sequence = record.at("sequence").get<int>();
		if (sequence < next_sequence)
			continue;
		if (sequence > next_sequence)
		{
			fail(out_error, error_kind::workflow,
				 "journal has a future completion at sequence " + std::to_string(sequence));
			return nullptr;
		}
		const bool state_already_applied =
		  state.value("last_completed_execution_id", std::string{}) ==
		  record.at("execution_id").get<std::string>();
		if (record.at("stage").get<std::string>() != current_stage && !state_already_applied)
		{
			fail(out_error, error_kind::workflow,
				 "journal completion does not match current stage '" + current_stage + "'");
			return nullptr;
		}
		if (found != nullptr)
		{
			fail(out_error, error_kind::workflow,
				 "journal has multiple completions at sequence " + std::to_string(next_sequence));
			return nullptr;
		}
		found = &record;
	}
	return found;
}

static bool engine_acknowledged(const json &state, const std::string &execution_id)
{
	for (const json &value : state.value("acknowledged_interruptions", json::array()))
	{
		if (value.is_string() && value == execution_id)
			return true;
	}
	return false;
}

static const json *engine_find_unmatched_start(const std::vector<json> &journal,
											   const json &state,
											   const json &context)
{
	const int next_sequence = static_cast<int>(context.at("history").size()) + 1;
	const std::string current_stage = state.at("current_stage").get<std::string>();
	for (auto record = journal.rbegin(); record != journal.rend(); ++record)
	{
		if (record->value("type", std::string{}) != "stage_started" ||
			record->value("sequence", 0) != next_sequence ||
			record->value("stage", std::string{}) != current_stage ||
			engine_acknowledged(state, record->value("execution_id", std::string{})))
			continue;
		if (engine_find_completion(journal, record->value("execution_id", std::string{})) == nullptr)
			return &*record;
	}
	return nullptr;
}

static bool engine_recover_legacy_event(engine &e,
										const fs::path &run_dir,
										const json &stage,
										json &context,
										json &state,
										odin_error &out_error)
{
	const int sequence = static_cast<int>(context.at("history").size()) + 1;
	char prefix[32];
	std::snprintf(prefix, sizeof(prefix), "%03d-", sequence);
	const fs::path path = run_dir / "events" /
						  (std::string(prefix) + stage.at("id").get<std::string>() + ".json");
	if (!fs::exists(path))
		return false;

	json record = json_read(path, out_error);
	if (failed(out_error))
		return false;
	if (record.value("sequence", 0) != sequence ||
		record.value("stage", std::string{}) != stage.at("id").get<std::string>() ||
		!record.contains("attempt") || !record.at("attempt").is_number_integer() ||
		!record.contains("kind") || record.at("kind") != stage.at("kind") ||
		!record.contains("result") || !record.at("result").is_object() ||
		!record.contains("metadata") || !record.at("metadata").is_object())
	{
		fail(out_error, error_kind::workflow, "invalid legacy event " + file_path_utf8(path));
		return false;
	}
	contract_validate(*e.service, record.at("result"), "handoff", file_path_utf8(path), out_error);
	if (failed(out_error))
		return false;
	json completed = record;
	completed["execution_id"] = "legacy-event-" + std::to_string(sequence);
	completed["journal_version"] = 1;
	completed["type"] = "stage_completed";
	state["stage_attempts"][stage.at("id").get<std::string>()] = record.at("attempt");
	return engine_apply_completion(run_dir, stage, completed, context, state, out_error);
}

static bool engine_recover_legacy_context(engine &e,
										  const fs::path &run_dir,
										  const std::map<std::string, const json *> &stages,
										  json &context,
										  json &state,
										  odin_error &out_error)
{
	const int transitions = state.at("transitions").get<int>();
	const int completed_count = static_cast<int>(context.at("history").size());
	if (completed_count == transitions)
		return false;
	if (completed_count != transitions + 1 || context.at("history").empty())
	{
		fail(out_error, error_kind::workflow, "legacy run state and context are inconsistent");
		return false;
	}

	const json &record = context.at("history").back();
	const std::string stage_id = state.at("current_stage").get<std::string>();
	const auto found = stages.find(stage_id);
	if (found == stages.end() || record.value("stage", std::string{}) != stage_id ||
		record.value("sequence", 0) != completed_count || !record.contains("attempt") ||
		!record.at("attempt").is_number_integer() || !record.contains("kind") ||
		record.at("kind") != found->second->at("kind") || !record.contains("result") ||
		!record.at("result").is_object() || !record.contains("metadata") ||
		!record.at("metadata").is_object())
	{
		fail(out_error, error_kind::workflow,
			 "legacy context completion does not match current stage '" + stage_id + "'");
		return false;
	}
	contract_validate(*e.service, record.at("result"), "handoff", "legacy context history",
					  out_error);
	if (failed(out_error))
		return false;

	json completed = record;
	completed["execution_id"] = "legacy-context-" + std::to_string(completed_count);
	completed["journal_version"] = 1;
	completed["type"] = "stage_completed";
	state["stage_attempts"][stage_id] = record.at("attempt");
	return engine_apply_completion(run_dir, *found->second, completed, context, state, out_error);
}

json engine_run(engine &e,
				const fs::path &run_dir,
				const engine_run_options &options,
				odin_error &out_error)
{
	run_lock lock;
	if (!run_lock_acquire(run_dir, lock, out_error))
		return json::object();

	json state = json_read(run_dir / "state.json", out_error);
	if (failed(out_error))
		return state;
	json context = json_read(run_dir / "context.json", out_error);
	if (failed(out_error))
		return state;

	const auto version = state.find("schema_version");
	if (version == state.end() || !version->is_number_integer())
	{
		fail(out_error, error_kind::workflow, "run state has an invalid schema version");
		return state;
	}
	int schema_version = version->get<int>();
	if (schema_version < 1 || schema_version > 2)
	{
		fail(out_error, error_kind::workflow,
			 "unsupported run schema version " + std::to_string(schema_version));
		return state;
	}
	if (engine_is_terminal(state.value("status", std::string{})) &&
		state.value("reason_code", std::string{}) != "outcome_uncertain" &&
		state.value("transitions", 0) == static_cast<int>(context.at("history").size()))
		return state;

	const json *workflow =
	  definitions_load_workflow(*e.defs, state.at("workflow").get<std::string>(), out_error);
	if (workflow == nullptr)
		return state;

	std::map<std::string, const json *> stages;
	for (const json &stage : workflow->at("stages"))
	{
		stages.emplace(stage.at("id").get<std::string>(), &stage);
	}

	std::vector<json> journal;
	if (!run_journal_load(run_dir, journal, out_error))
		return state;
	if (schema_version == 1 &&
		engine_recover_legacy_context(e, run_dir, stages, context, state, out_error))
	{
		schema_version = 2;
	}
	if (failed(out_error))
		return state;

	const json *next_completion = engine_find_next_completion(journal, state, context, out_error);
	if (failed(out_error))
		return state;
	if (next_completion != nullptr)
	{
		const std::string stage_id = next_completion->at("stage").get<std::string>();
		const auto found = stages.find(stage_id);
		if (found == stages.end() ||
			!engine_apply_completion(run_dir, *found->second, *next_completion, context, state, out_error))
			return state;
	}
	else if (schema_version == 1)
	{
		const std::string stage_id = state.at("current_stage").get<std::string>();
		const auto found = stages.find(stage_id);
		if (found != stages.end() &&
			engine_recover_legacy_event(e, run_dir, *found->second, context, state, out_error))
		{
			schema_version = 2;
			journal.clear();
		}
		if (failed(out_error))
			return state;
	}

	const json *interrupted = nullptr;
	if (state.contains("in_progress") && state.at("in_progress").is_object())
	{
		const std::string execution_id =
		  state.at("in_progress").value("execution_id", std::string{});
		const json *completed = engine_find_completion(journal, execution_id);
		if (completed != nullptr)
		{
			const std::string stage_id = completed->at("stage").get<std::string>();
			const auto found = stages.find(stage_id);
			if (found == stages.end() ||
				!engine_apply_completion(run_dir, *found->second, *completed, context, state, out_error))
				return state;
		}
		else
		{
			interrupted = &state.at("in_progress");
		}
	}
	else
	{
		interrupted = engine_find_unmatched_start(journal, state, context);
		if (interrupted != nullptr)
		{
			state["in_progress"] = *interrupted;
			state["stage_attempts"][interrupted->at("stage").get<std::string>()] =
			  interrupted->at("attempt");
		}
		else if (schema_version == 1)
		{
			const std::string stage_id = state.at("current_stage").get<std::string>();
			const int unknown_attempt = state["stage_attempts"].value(stage_id, 0) + 1;
			state["in_progress"] = json{{"stage", state.at("current_stage")},
										{"attempt", unknown_attempt},
										{"execution_id", "legacy-unknown"}};
			state["stage_attempts"][stage_id] = unknown_attempt;
			interrupted = &state.at("in_progress");
		}
	}

	if (interrupted != nullptr || state.value("reason_code", std::string{}) == "outcome_uncertain")
	{
		if (!options.retry_interrupted)
		{
			const std::string stage_id = state.at("in_progress").at("stage").get<std::string>();
			const int attempt = state.at("in_progress").value("attempt", 0);
			state["schema_version"] = 2;
			state["status"] = "blocked";
			state["reason_code"] = "outcome_uncertain";
			state["reason"] = "stage '" + stage_id + "' attempt " + std::to_string(attempt) +
							  " may have completed before interruption; resume with --retry-interrupted to retry";
			state["updated_at"] = engine_utc_now();
			json_write_atomic(run_dir / "state.json", state, out_error);
			return state;
		}
		state["schema_version"] = 2;
		json acknowledgements = state.value("acknowledged_interruptions", json::array());
		const std::string interrupted_id =
		  state.at("in_progress").value("execution_id", std::string{});
		if (!interrupted_id.empty())
			acknowledgements.push_back(interrupted_id);
		state["acknowledged_interruptions"] = acknowledgements;
		state["status"] = "running";
		state.erase("reason");
		state.erase("reason_code");
		state.erase("in_progress");
		state["updated_at"] = engine_utc_now();
		json_write_atomic(run_dir / "state.json", state, out_error);
		if (failed(out_error))
			return state;
	}

	while (!engine_is_terminal(state.at("status").get<std::string>()))
	{
		if (state.at("transitions").get<int>() >= e.config.max_total_transitions)
		{
			state["status"] = "blocked";
			state["reason"] = "maximum total transitions reached";
			break;
		}

		const std::string stage_id = state.at("current_stage").get<std::string>();
		const auto found = stages.find(stage_id);
		if (found == stages.end())
		{
			fail(out_error, error_kind::workflow,
				 "workflow references unknown stage '" + stage_id + "'");
			return state;
		}
		const json &stage = *found->second;

		const int attempts = state["stage_attempts"].value(stage_id, 0) + 1;
		state["stage_attempts"][stage_id] = attempts;
		if (attempts > stage.value("max_attempts", 3))
		{
			state["status"] = "blocked";
			state["reason"] = "stage '" + stage_id + "' exceeded its attempt limit";
			break;
		}

		const int sequence = static_cast<int>(context.at("history").size()) + 1;
		const std::string execution_id = engine_execution_id();
		const std::string started_at = engine_utc_now();
		json started{{"journal_version", 1},
					 {"type", "stage_started"},
					 {"execution_id", execution_id},
					 {"sequence", sequence},
					 {"stage", stage_id},
					 {"kind", stage.at("kind")},
					 {"attempt", attempts},
					 {"at", started_at}};
		if (!run_journal_publish(run_dir, started, out_error))
			return state;
		state["schema_version"] = 2;
		state["in_progress"] = json{{"execution_id", execution_id},
									{"sequence", sequence},
									{"stage", stage_id},
									{"kind", stage.at("kind")},
									{"attempt", attempts},
									{"started_at", started_at}};
		state["updated_at"] = started_at;
		json_write_atomic(run_dir / "state.json", state, out_error);
		if (failed(out_error))
			return state;

		stage_outcome outcome;
		if (!engine_execute_stage(e, stage, context, options.model_override, outcome, out_error))
		{
			return state;
		}

		contract_validate(*e.service, outcome.result, "handoff", "stage '" + stage_id + "' output",
						  out_error);
		if (failed(out_error))
			return state;

		json completed{{"journal_version", 1},
					   {"type", "stage_completed"},
					   {"execution_id", execution_id},
					   {"sequence", sequence},
					   {"stage", stage_id},
					   {"kind", stage.at("kind")},
					   {"attempt", attempts},
					   {"at", engine_utc_now()},
					   {"result", outcome.result},
					   {"metadata", outcome.metadata}};
		if (!run_journal_publish(run_dir, completed, out_error))
			return state;
		if (!engine_apply_completion(run_dir, stage, completed, context, state, out_error))
			return state;
	}

	state["updated_at"] = engine_utc_now();
	json_write_atomic(run_dir / "context.json", context, out_error);
	if (failed(out_error))
		return state;
	json_write_atomic(run_dir / "state.json", state, out_error);
	return state;
}

json engine_run(engine &e,
				const fs::path &run_dir,
				const std::string &model_override,
				odin_error &out_error)
{
	return engine_run(e, run_dir, engine_run_options{model_override, false}, out_error);
}
