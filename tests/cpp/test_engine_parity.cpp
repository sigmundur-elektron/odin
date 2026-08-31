#include <doctest/doctest.h>

#include "atomic_file.h"
#include "config.h"
#include "definitions.h"
#include "engine.h"
#include "json_io.h"
#include "sidecar.h"
#include "subprocess.h"
#include "test_support.h"

#include <string>

namespace fs = std::filesystem;

// differential parity against the python engine.
//
// both engines are pointed at the same odin.toml and the same task, then every
// byte they wrote is compared. only the genuinely volatile fields are blanked:
// ids, timestamps and timings cannot agree between two runs and say nothing
// about correctness. everything else - stage order, summaries, artifacts,
// attempt counts, transition counts - must match exactly.
static void normalise(json &value, const std::string &key)
{
	if (value.is_object())
	{
		for (auto entry = value.begin(); entry != value.end(); ++entry)
		{
			normalise(entry.value(), entry.key());
		}
		return;
	}
	if (value.is_array())
	{
		for (json &item : value) normalise(item, key);
		return;
	}

	static const char *volatile_keys[] = {"run_id", "run_dir", "task_file",
										  "created_at", "updated_at", "at",
										  "duration_seconds"};
	for (const char *name : volatile_keys)
	{
		if (key == name)
		{
			value = "<normalised>";
			return;
		}
	}
}

TEST_CASE("the c++ engine and the python engine produce the same durable state")
{
	const temp_dir dir;
	const std::string mock = (fs::path(ODIN_REPO_ROOT) / "scripts" / "mock_agent.py").generic_string();

	const auto config_path = dir.path / "odin.toml";
	temp_write(config_path,
			   "[harness]\nstate_dir = \".odin/runs\"\nmax_total_transitions = 30\n"
			   "[git]\nstage_on_success = false\n"
			   "[adapters.mock]\ncommand = [\"python\", \"" +
				 mock + "\", \"--model\", \"{model}\"]\n"
						"[models.test]\nadapter = \"mock\"\nmodel = \"fixture\"\nparameter_billions = 0\n"
						"[routing]\ndefault = \"test\"\n"
						"[gates.quality]\ncommand = [\"python\", \"-c\", \"raise SystemExit(0)\"]\n");

	const auto task_path = dir.path / "task.json";
	const json task = json{{"id", "feature-1"},
						   {"kind", "feature"},
						   {"title", "Feature"},
						   {"request", "Add it."}};
	temp_write(task_path, task.dump());

	// --- the port ---
	odin_error err;
	const project_config config = config_load(config_path, err);
	REQUIRE_FALSE(failed(err));

	sidecar service;
	sidecar_configure(service, fs::path(ODIN_REPO_ROOT), "");
	definitions defs;
	definitions_configure(defs, service, fs::path(ODIN_REPO_ROOT) / "harness");
	engine machine;
	engine_configure(machine, config, defs, service);

	const fs::path cpp_run = engine_create_run(machine, task, task_path, err);
	REQUIRE_FALSE(failed(err));
	engine_run(machine, cpp_run, "", err);
	REQUIRE_FALSE(failed(err));
	sidecar_stop(service);

	// --- the reference ---
	subprocess_options options;
	options.command = {"python",
					   (fs::path(ODIN_REPO_ROOT) / "scripts" / "reference_run.py").string(),
					   config_path.string(), task_path.string()};
	options.working_directory = fs::path(ODIN_REPO_ROOT);
	options.timeout_seconds = 300;

	const subprocess_result reference = subprocess_run(options, err);
	REQUIRE_FALSE(failed(err));
	REQUIRE_MESSAGE(reference.exit_code == 0, reference.stderr_text);

	std::string printed = reference.stdout_text;
	while (!printed.empty() && (printed.back() == '\n' || printed.back() == '\r')) printed.pop_back();
	const fs::path py_run = printed;
	REQUIRE(fs::exists(py_run / "state.json"));

	// --- compare ---
	for (const char *name : {"state.json", "context.json", "task.json"})
	{
		CAPTURE(name);
		json mine = json_read(cpp_run / name, err);
		REQUIRE_FALSE(failed(err));
		json theirs = json_read(py_run / name, err);
		REQUIRE_FALSE(failed(err));

		normalise(mine, "");
		normalise(theirs, "");
		if (std::string(name) == "state.json")
		{
			mine.erase("schema_version");
			theirs.erase("schema_version");
			mine.erase("last_completed_execution_id");
			theirs.erase("last_completed_execution_id");
		}
		CHECK(mine == theirs);
	}

	// the event log must line up file for file, byte for byte after normalising
	std::vector<std::string> mine_events;
	std::vector<std::string> their_events;
	for (const auto &item : fs::directory_iterator(cpp_run / "events"))
	{
		mine_events.push_back(item.path().filename().string());
	}
	for (const auto &item : fs::directory_iterator(py_run / "events"))
	{
		their_events.push_back(item.path().filename().string());
	}
	std::sort(mine_events.begin(), mine_events.end());
	std::sort(their_events.begin(), their_events.end());
	CHECK(mine_events == their_events);

	for (const std::string &name : mine_events)
	{
		CAPTURE(name);
		json mine = json_read(cpp_run / "events" / name, err);
		REQUIRE_FALSE(failed(err));
		json theirs = json_read(py_run / "events" / name, err);
		REQUIRE_FALSE(failed(err));

		normalise(mine, "");
		normalise(theirs, "");
		CHECK(mine == theirs);
	}
}
