#include "engine.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

#include "adapter.h"
#include "atomic_file.h"
#include "contracts.h"
#include "json_io.h"
#include "subprocess.h"

namespace fs = std::filesystem;

// what _execute_stage returns in python: the handoff plus the metadata recorded
// alongside it in the event log.
struct stage_outcome
{
	json result;
	json metadata;
};

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

	if (!e.config.stage_on_success)
	{
		out_outcome.result = json{
		  {"status", "approved"},
		  {"summary", "explicit staging manifest prepared; automatic staging is disabled"},
		  {"artifacts",
		   json{{"staged_files", json::array()}, {"staging_manifest", candidates}}},
		  {"findings", json::array()}};
		out_outcome.metadata =
		  json{{"operation", "git-stage"}, {"executed", false}};
		return true;
	}

	subprocess_options options;
	options.command = {"git", "add", "--"};
	for (const json &path : candidates) options.command.push_back(path.get<std::string>());
	options.working_directory = e.config.root;
	options.merge_stderr = true;
	// harness/engine.py passes no timeout here, and neither do we
	options.timeout_seconds = 0;

	odin_error run_error;
	const subprocess_result completed = subprocess_run(options, run_error);
	const int exit_code = failed(run_error) ? 1 : completed.exit_code;
	const std::string output = failed(run_error) ? run_error.message : completed.stdout_text;

	out_outcome.result =
	  json{{"status", exit_code == 0 ? "approved" : "blocked"},
		   {"summary", "explicit git staging exited " + std::to_string(exit_code)},
		   {"artifacts", json{{"staged_files", candidates}, {"output", output}}},
		   {"findings", json::array()}};

	if (exit_code != 0)
	{
		std::string trimmed = output;
		const std::size_t first = trimmed.find_first_not_of(" \t\n\r\f\v");
		const std::size_t last = trimmed.find_last_not_of(" \t\n\r\f\v");
		trimmed = first == std::string::npos ? "" : trimmed.substr(first, last - first + 1);
		out_outcome.result["findings"] = json::array({trimmed});
	}
	out_outcome.metadata = json{{"operation", "git-stage"}, {"exit_code", exit_code}};
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

	const std::string run_id = engine_local_stamp() + "-" + task.at("id").get<std::string>() + "-" +
							   engine_run_suffix();
	const fs::path run_dir = e.config.state_dir / run_id;

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
	state["schema_version"] = 1;
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

json engine_run(engine &e,
				const fs::path &run_dir,
				const std::string &model_override,
				odin_error &out_error)
{
	json state = json_read(run_dir / "state.json", out_error);
	if (failed(out_error))
		return state;
	json context = json_read(run_dir / "context.json", out_error);
	if (failed(out_error))
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

		stage_outcome outcome;
		if (!engine_execute_stage(e, stage, context, model_override, outcome, out_error))
		{
			return state;
		}

		contract_validate(*e.service, outcome.result, "handoff", "stage '" + stage_id + "' output",
						  out_error);
		if (failed(out_error))
			return state;

		const int sequence = static_cast<int>(context.at("history").size()) + 1;
		json record;
		record["sequence"] = sequence;
		record["stage"] = stage_id;
		record["kind"] = stage.at("kind");
		record["attempt"] = attempts;
		record["at"] = engine_utc_now();
		record["result"] = outcome.result;
		record["metadata"] = outcome.metadata;

		context["history"].push_back(record);
		context["artifacts"][stage.value("output", stage_id)] = outcome.result;

		char name[32];
		std::snprintf(name, sizeof(name), "%03d-", sequence);
		json_write_atomic(run_dir / "events" / (std::string(name) + stage_id + ".json"), record,
						  out_error);
		if (failed(out_error))
			return state;

		const std::string outcome_status = outcome.result.at("status").get<std::string>();
		const json transitions = stage.value("on", json::object());
		const std::string next = transitions.value(outcome_status, std::string{});
		if (next.empty())
		{
			state["status"] = "blocked";
			state["reason"] =
			  "stage '" + stage_id + "' has no transition for '" + outcome_status + "'";
			break;
		}

		state["transitions"] = state.at("transitions").get<int>() + 1;
		if (engine_is_terminal(next))
		{
			state["status"] = next;
			state["current_stage"] = stage_id;
		}
		else
		{
			state["current_stage"] = next;
		}
		state["updated_at"] = engine_utc_now();

		json_write_atomic(run_dir / "context.json", context, out_error);
		if (failed(out_error))
			return state;
		json_write_atomic(run_dir / "state.json", state, out_error);
		if (failed(out_error))
			return state;
	}

	state["updated_at"] = engine_utc_now();
	json_write_atomic(run_dir / "context.json", context, out_error);
	if (failed(out_error))
		return state;
	json_write_atomic(run_dir / "state.json", state, out_error);
	return state;
}
