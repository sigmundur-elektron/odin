#include "cli.h"

#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <CLI/CLI.hpp>

#include "adapter.h"
#include "atomic_file.h"
#include "config.h"
#include "contracts.h"
#include "definitions.h"
#include "delegate.h"
#include "engine.h"
#include "json_io.h"
#include "runtime_paths.h"
#include "sidecar.h"

namespace fs = std::filesystem;

// harness/cli.py prints with json.dumps(..., indent=2), whose default is
// sort_keys=False. odin sorts here instead, matching what it writes to disk.
//
// this is a deliberate divergence and the only one on stdout: key ORDER differs
// for `start` and `benchmark`, whose python output follows dict insertion order.
// json has no ordering semantics, the state files themselves are sorted, and the
// cli parity harness compares parsed values rather than bytes.
//
// the trailing "\n" becomes "\r\n" on windows through the CRT's text-mode
// stdout, which is exactly what python's print() does.
static void cli_print(const json &value)
{
	std::printf("%s\n", value.dump(2, ' ', true, json::error_handler_t::replace).c_str());
}

static int cli_fail(const odin_error &error)
{
	std::fprintf(stderr, "odin: %s\n", error.message.c_str());
	return 2;
}

// python: dt.datetime.now().strftime('%Y%m%d-%H%M%S'), local time
static std::string cli_local_stamp()
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

// ---------------------------------------------------------------- helpers

static bool cli_overrides(const std::vector<std::string> &items,
						  std::map<std::string, std::string> &out_changes,
						  odin_error &out_error)
{
	for (const std::string &item : items)
	{
		const std::size_t split = item.find('=');
		if (split == std::string::npos || split == 0)
		{
			fail(out_error, error_kind::config,
				 "invalid --set value '" + item + "', expected KEY=VALUE");
			return false;
		}
		out_changes[item.substr(0, split)] = item.substr(split + 1);
	}
	return true;
}

// a template is either a path to a task file or the name of a bundled one
static bool cli_task(engine &machine,
					 const std::string &name,
					 const std::map<std::string, std::string> &changes,
					 json &out_task,
					 fs::path &out_path,
					 odin_error &out_error)
{
	const fs::path source = name;
	if (fs::exists(source))
	{
		out_task = json_read(source, out_error);
		if (failed(out_error))
			return false;

		std::error_code code;
		fs::path resolved = fs::weakly_canonical(source, code);
		out_path = code ? fs::absolute(source) : resolved;
	}
	else
	{
		const json *bundled = definitions_load_template(*machine.defs, name, out_error);
		if (bundled == nullptr)
			return false;
		out_task = *bundled;
		// python builds a marker path here rather than a real one
		out_path = fs::path("built-in:" + name);
	}

	for (const auto &[key, value] : changes) out_task[key] = value;

	contract_validate(*machine.service, out_task, "task", file_path_utf8(out_path), out_error);
	return !failed(out_error);
}

static fs::path cli_run_dir(const project_config &config, const std::string &value)
{
	const fs::path candidate = value;
	if (fs::is_directory(candidate))
	{
		std::error_code code;
		const fs::path resolved = fs::weakly_canonical(candidate, code);
		return code ? fs::absolute(candidate) : resolved;
	}
	return config.state_dir / value;
}

static int cli_report_run(const fs::path &run_dir, const json &state, bool always_zero)
{
	json payload;
	payload["run_dir"] = file_path_utf8(run_dir);
	payload["state"] = state;
	cli_print(payload);

	if (always_zero)
		return 0;
	return state.value("status", std::string{}) == "complete" ? 0 : 2;
}

// --------------------------------------------------------------- commands

static int cli_models(const project_config &config, bool as_json)
{
	json profiles = json::array();
	for (const auto &[name, profile] : config.models)
	{
		json entry;
		entry["profile"] = name;
		entry["adapter"] = profile.adapter;
		entry["model"] = profile.model;
		entry["parameter_billions"] = profile.parameter_billions;
		entry["tags"] = profile.tags;
		entry["adapter_configured"] = config.adapters.count(profile.adapter) > 0;
		profiles.push_back(entry);
	}

	if (as_json)
	{
		json payload;
		payload["models"] = profiles;
		payload["routing"] = config.routing;
		cli_print(payload);
		return 0;
	}

	if (profiles.empty())
	{
		std::printf("No model profiles configured. Run: odin doctor --emit-config\n");
		return 1;
	}

	for (const json &item : profiles)
	{
		// python treats 0 as falsy here, so a zero-parameter profile shows no size
		const json &size = item.at("parameter_billions");
		const bool show = !size.is_null() && size != 0;
		const std::string suffix = show ? "  ~" + size.dump() + "B" : "";
		const std::string flag =
		  item.at("adapter_configured").get<bool>() ? "" : "  [adapter missing]";

		std::printf("%-28s %-16s %s%s%s\n", item.at("profile").get<std::string>().c_str(),
					item.at("adapter").get<std::string>().c_str(),
					item.at("model").get<std::string>().c_str(), suffix.c_str(), flag.c_str());
	}
	std::printf("\n");
	for (const auto &[role, profile] : config.routing)
	{
		std::printf("routing.%-20s -> %s\n", role.c_str(), profile.c_str());
	}
	return 0;
}

// walk the bundled definitions and cross-check every reference between them
static int cli_validate(engine &machine, odin_error &out_error)
{
	const fs::path package_root = machine.defs->package_root;

	const std::pair<const char *, const char *> kinds[] = {{"agents", "agent"},
														   {"skills", "skill"},
														   {"workflows", "workflow"},
														   {"templates", "task"}};

	std::map<std::string, std::map<std::string, json>> loaded;
	json counts = json::object();

	for (const auto &[kind, contract] : kinds)
	{
		const fs::path directory = package_root / kind;
		std::vector<std::string> identifiers;
		if (fs::is_directory(directory))
		{
			for (const auto &item : fs::directory_iterator(directory))
			{
				if (item.path().extension() == ".json")
				{
					identifiers.push_back(item.path().stem().string());
				}
			}
		}

		for (const std::string &identifier : identifiers)
		{
			const json *value = nullptr;
			const std::string name(kind);
			if (name == "agents")
				value = definitions_load_agent(*machine.defs, identifier, out_error);
			else if (name == "skills")
				value = definitions_load_skill(*machine.defs, identifier, out_error);
			else if (name == "workflows")
				value = definitions_load_workflow(*machine.defs, identifier, out_error);
			else
				value = definitions_load_template(*machine.defs, identifier, out_error);

			if (value == nullptr)
				return cli_fail(out_error);
			loaded[kind][identifier] = *value;
		}
		counts[kind] = identifiers.size();
	}

	for (const auto &[identifier, agent] : loaded["agents"])
	{
		std::vector<std::string> missing;
		for (const json &skill : agent.value("skills", json::array()))
		{
			const std::string name = skill.get<std::string>();
			if (loaded["skills"].count(name) == 0)
				missing.push_back(name);
		}
		if (!missing.empty())
		{
			std::string joined;
			for (std::size_t i = 0; i < missing.size(); ++i)
			{
				if (i > 0)
					joined += ", ";
				joined += missing[i];
			}
			fail(out_error, error_kind::config,
				 "agent '" + identifier + "' references missing skills: " + joined);
			return cli_fail(out_error);
		}
	}

	for (const auto &[identifier, workflow] : loaded["workflows"])
	{
		std::vector<std::string> stage_ids;
		for (const json &stage : workflow.at("stages"))
		{
			stage_ids.push_back(stage.at("id").get<std::string>());
		}
		const auto known = [&](const std::string &value) {
			for (const std::string &id : stage_ids)
			{
				if (id == value)
					return true;
			}
			return false;
		};

		const std::string start = workflow.at("start").get<std::string>();
		if (!known(start))
		{
			fail(out_error, error_kind::config,
				 "workflow '" + identifier + "' starts at missing stage '" + start + "'");
			return cli_fail(out_error);
		}

		for (const json &stage : workflow.at("stages"))
		{
			const std::string agent = stage.value("agent", std::string{});
			if (!agent.empty() && loaded["agents"].count(agent) == 0)
			{
				fail(out_error, error_kind::config,
					 "workflow '" + identifier + "' references missing agent '" + agent + "'");
				return cli_fail(out_error);
			}

			std::vector<std::string> invalid;
			for (const auto &entry : stage.at("on").items())
			{
				const std::string target = entry.value().get<std::string>();
				if (!known(target) && !engine_is_terminal(target))
					invalid.push_back(target);
			}
			if (!invalid.empty())
			{
				std::sort(invalid.begin(), invalid.end());
				std::string joined;
				for (std::size_t i = 0; i < invalid.size(); ++i)
				{
					if (i > 0)
						joined += ", ";
					joined += invalid[i];
				}
				fail(out_error, error_kind::config,
					 "workflow '" + identifier + "' stage '" +
					   stage.at("id").get<std::string>() +
					   "' has invalid transitions: " + joined);
				return cli_fail(out_error);
			}
		}
	}

	json payload;
	payload["status"] = "valid";
	payload["counts"] = counts;
	cli_print(payload);
	return 0;
}

static int cli_benchmark(engine &machine,
						 const json &task,
						 const fs::path &task_file,
						 const std::vector<std::string> &model_names,
						 odin_error &out_error)
{
	json results = json::array();
	bool all_complete = true;

	for (const std::string &name : model_names)
	{
		const model_profile *profile = config_model_for(machine.config, "benchmark", name, out_error);
		if (profile == nullptr)
			return cli_fail(out_error);

		const auto started = std::chrono::steady_clock::now();
		const fs::path run_dir = engine_create_run(machine, task, task_file, out_error);
		if (failed(out_error))
			return cli_fail(out_error);

		const json state = engine_run(machine, run_dir, name, out_error);
		if (failed(out_error))
			return cli_fail(out_error);

		const auto elapsed =
		  std::chrono::duration<double>(std::chrono::steady_clock::now() - started);

		json entry;
		entry["profile"] = name;
		entry["model"] = profile->model;
		entry["parameter_billions"] = profile->parameter_billions;
		entry["tags"] = profile->tags;
		entry["status"] = state.at("status");
		entry["transitions"] = state.at("transitions");
		entry["duration_seconds"] = std::round(elapsed.count() * 1e6) / 1e6;
		entry["run_dir"] = file_path_utf8(run_dir);
		results.push_back(entry);

		if (state.at("status") != "complete")
			all_complete = false;
	}

	const fs::path report_path = machine.config.root / ".odin" / "benchmarks" /
								 (cli_local_stamp() + "-" + task.at("id").get<std::string>() +
								  ".json");

	json report;
	report["task"] = task;
	report["results"] = results;
	json_write_atomic(report_path, report, out_error);
	if (failed(out_error))
		return cli_fail(out_error);

	json payload = report;
	payload["report"] = file_path_utf8(report_path);
	cli_print(payload);
	return all_complete ? 0 : 2;
}

// ------------------------------------------------------------------ main

// the first non-option token, so doctor/tools/auth can be forwarded before
// CLI11 ever sees flags it does not know about.
static std::string cli_leading_command(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string part = argv[i];
		if (part == "--config")
		{
			++i; // skip its value
			continue;
		}
		if (part.rfind("--config=", 0) == 0)
			continue;
		if (!part.empty() && part[0] == '-')
			continue;
		return part;
	}
	return {};
}

// --config's value, so root can be derived before any real parsing happens
static std::string cli_config_argument(int argc, char **argv)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string part = argv[i];
		if (part == "--config" && i + 1 < argc)
			return argv[i + 1];
		if (part.rfind("--config=", 0) == 0)
			return part.substr(9);
	}
	return "odin.toml";
}

static std::vector<std::string> cli_forwarded_arguments(int argc, char **argv,
														const fs::path &resolved_config)
{
	std::vector<std::string> forwarded;
	forwarded.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
	for (int i = 1; i < argc; ++i)
	{
		const std::string part = argv[i];
		if (part == "--config" && i + 1 < argc)
		{
			forwarded.push_back(part);
			forwarded.push_back(file_path_utf8(resolved_config));
			++i;
		}
		else if (part.rfind("--config=", 0) == 0)
		{
			forwarded.push_back("--config=" + file_path_utf8(resolved_config));
		}
		else
		{
			forwarded.push_back(part);
		}
	}
	return forwarded;
}

int cli_main(int argc, char **argv)
{
	const std::string config_argument = cli_config_argument(argc, argv);
	const runtime_paths paths = runtime_paths_resolve(argc > 0 ? argv[0] : nullptr,
													  config_argument);
	const std::string leading = cli_leading_command(argc, argv);
	if (delegate_owns(leading))
	{
		const std::vector<std::string> forwarded =
		  cli_forwarded_arguments(argc, argv, paths.config_path);
		odin_error error;
		const int status = delegate_to_python(sidecar_default_interpreter(),
											  paths.runtime_root,
											  paths.project_root,
											  forwarded, error);
		if (failed(error))
			return cli_fail(error);
		return status;
	}

	CLI::App app{"Contract-driven development workflow harness", "odin"};
	app.require_subcommand(1);

	std::string config_path = "odin.toml";
	app.add_option("--config", config_path, "project configuration (default: odin.toml)");

	std::string template_name;
	std::string run_name;
	std::string model;
	std::vector<std::string> sets;
	std::vector<std::string> benchmark_models;
	bool self_only = false;
	bool as_json = false;
	bool retry_interrupted = false;

	CLI::App *start = app.add_subcommand("start", "run a feature or bug-fix template");
	start->add_option("template", template_name, "template JSON path, or built-in name")->required();
	start->add_option("--set", sets, "override a top-level template value")->type_name("KEY=VALUE");
	start->add_option("--model", model, "force one configured model profile");

	CLI::App *resume = app.add_subcommand("resume", "continue an existing run");
	resume->add_option("run", run_name, "run id or run directory")->required();
	resume->add_option("--model", model, "force one configured model profile");
	resume->add_flag("--retry-interrupted", retry_interrupted,
					 "acknowledge and retry a stage whose outcome is uncertain");

	CLI::App *status = app.add_subcommand("status", "show one run's durable state");
	status->add_option("run", run_name, "run id or run directory")->required();

	CLI::App *benchmark = app.add_subcommand("benchmark", "compare model profiles on one task");
	benchmark->add_option("template", template_name)->required();
	benchmark->add_option("--models", benchmark_models)->required()->expected(1, -1);
	benchmark->add_option("--set", sets)->type_name("KEY=VALUE");

	CLI::App *validate = app.add_subcommand("validate", "validate bundled definitions");
	validate->add_flag("--self-only", self_only, "skip project configuration");

	CLI::App *models = app.add_subcommand("models", "list configured model profiles");
	models->add_flag("--json", as_json);

	// declared so `--help` matches the python cli even though they are forwarded
	app.add_subcommand("doctor", "probe the machine for reachable model providers");
	app.add_subcommand("auth", "store provider credentials");
	app.add_subcommand("tools", "install an agent CLI into .odin/tools");

	try
	{
		app.parse(argc, argv);
	}
	catch (const CLI::ParseError &error)
	{
		return app.exit(error);
	}

	odin_error err;
	const fs::path resolved_config = config_path;

	sidecar service;
	sidecar_configure(service, paths.runtime_root, "");
	definitions defs;
	definitions_configure(defs, service, paths.runtime_root / "harness");

	engine machine;
	machine.defs = &defs;
	machine.service = &service;

	// `validate --self-only` is the one command that must work without a project
	if (validate->parsed())
	{
		if (!self_only)
		{
			machine.config = config_load(resolved_config, err);
			if (failed(err))
			{
				sidecar_stop(service);
				return cli_fail(err);
			}
		}
		const int status_code = cli_validate(machine, err);
		sidecar_stop(service);
		return status_code;
	}

	machine.config = config_load(resolved_config, err);
	if (failed(err))
	{
		sidecar_stop(service);
		return cli_fail(err);
	}

	if (models->parsed())
	{
		sidecar_stop(service);
		return cli_models(machine.config, as_json);
	}

	std::map<std::string, std::string> changes;
	if (!cli_overrides(sets, changes, err))
	{
		sidecar_stop(service);
		return cli_fail(err);
	}

	int exit_code = 2;
	if (start->parsed() || benchmark->parsed())
	{
		json task;
		fs::path task_file;
		if (!cli_task(machine, template_name, changes, task, task_file, err))
		{
			sidecar_stop(service);
			return cli_fail(err);
		}

		if (benchmark->parsed())
		{
			exit_code = cli_benchmark(machine, task, task_file, benchmark_models, err);
		}
		else
		{
			const fs::path run_dir = engine_create_run(machine, task, task_file, err);
			if (failed(err))
			{
				sidecar_stop(service);
				return cli_fail(err);
			}
			const json state = engine_run(machine, run_dir, model, err);
			if (failed(err))
			{
				sidecar_stop(service);
				return cli_fail(err);
			}
			exit_code = cli_report_run(run_dir, state, false);
		}
	}
	else if (resume->parsed())
	{
		const fs::path run_dir = cli_run_dir(machine.config, run_name);
		const json state =
		  engine_run(machine, run_dir, engine_run_options{model, retry_interrupted}, err);
		if (failed(err))
		{
			sidecar_stop(service);
			return cli_fail(err);
		}
		exit_code = cli_report_run(run_dir, state, false);
	}
	else if (status->parsed())
	{
		const fs::path run_dir = cli_run_dir(machine.config, run_name);
		const json state = json_read(run_dir / "state.json", err);
		if (failed(err))
		{
			sidecar_stop(service);
			return cli_fail(err);
		}
		exit_code = cli_report_run(run_dir, state, true);
	}

	sidecar_stop(service);
	return exit_code;
}
