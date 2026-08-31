#include <doctest/doctest.h>

#include "atomic_file.h"
#include "config.h"
#include "contracts.h"
#include "definitions.h"
#include "engine.h"
#include "json_io.h"
#include "sidecar.h"
#include "test_support.h"

#include <string>
#include <vector>

namespace fs = std::filesystem;

static fs::path repo_root()
{
	return fs::path(ODIN_REPO_ROOT);
}

// the same fixture tests/test_engine.py builds: a project whose only adapter is
// the deterministic mock agent, and whose gate exits with a configurable code.
static std::string engine_toml(int gate_exit)
{
	const std::string mock = (repo_root() / "scripts" / "mock_agent.py").generic_string();
	return "[harness]\n"
		   "state_dir = \".odin/runs\"\n"
		   "max_total_transitions = 30\n"
		   "[git]\n"
		   "stage_on_success = false\n"
		   "[adapters.mock]\n"
		   "command = [\"python\", \"" +
		   mock +
		   "\", \"--model\", \"{model}\"]\n"
		   "[models.test]\n"
		   "adapter = \"mock\"\n"
		   "model = \"fixture\"\n"
		   "parameter_billions = 0\n"
		   "[routing]\n"
		   "default = \"test\"\n"
		   "[gates.quality]\n"
		   "command = [\"python\", \"-c\", \"raise SystemExit(" +
		   std::to_string(gate_exit) + ")\"]\n";
}

// everything one run needs, torn down together so a failed assertion cannot
// strand a python child.
struct engine_fixture
{
	temp_dir dir;
	sidecar service;
	definitions defs;
	engine machine;

	explicit engine_fixture(int gate_exit = 0)
	{
		const auto config_path = dir.path / "odin.toml";
		temp_write(config_path, engine_toml(gate_exit));

		odin_error err;
		const project_config config = config_load(config_path, err);
		REQUIRE_FALSE(failed(err));

		sidecar_configure(service, repo_root(), "");
		definitions_configure(defs, service, repo_root() / "harness");
		engine_configure(machine, config, defs, service);
	}

	~engine_fixture() { sidecar_stop(service); }

	engine_fixture(const engine_fixture &) = delete;
	engine_fixture &operator=(const engine_fixture &) = delete;
};

static json feature_task()
{
	return json{{"id", "feature-1"},
				{"kind", "feature"},
				{"title", "Feature"},
				{"request", "Add it."}};
}

static std::vector<std::string> stage_sequence(const json &context)
{
	std::vector<std::string> stages;
	for (const json &event : context.at("history"))
	{
		stages.push_back(event.at("stage").get<std::string>());
	}
	return stages;
}

static std::size_t count_of(const std::vector<std::string> &values, const std::string &wanted)
{
	std::size_t total = 0;
	for (const std::string &value : values)
	{
		if (value == wanted)
			++total;
	}
	return total;
}

TEST_CASE("a feature run completes and keeps an event log")
{
	engine_fixture fixture;
	odin_error err;

	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "complete");

	const json context = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));

	const std::vector<std::string> expected = {"specify", "review-spec", "spec-checkpoint",
											   "implement", "quality-gate", "verify",
											   "finalize", "stage"};
	CHECK(stage_sequence(context) == expected);
	CHECK(context.at("artifacts").at("staging").at("artifacts").at("staging_manifest") ==
		  json::array({"README.md"}));
}

TEST_CASE("a bugfix run reproduces before specifying")
{
	engine_fixture fixture;
	odin_error err;

	const json task = json{
	  {"id", "bug-1"},
	  {"kind", "bugfix"},
	  {"title", "Bug"},
	  {"request", "Fix it."},
	  {"reproduction",
	   json{{"steps", json::array({"run it"})}, {"expected", "works"}, {"actual", "fails"}}}};

	const fs::path run_dir =
	  engine_create_run(fixture.machine, task, fixture.dir.path / "bug.json", err);
	REQUIRE_FALSE(failed(err));

	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "complete");

	const json context = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));

	const std::vector<std::string> stages = stage_sequence(context);
	REQUIRE_FALSE(stages.empty());
	CHECK(stages.front() == "reproduce");
	CHECK(count_of(stages, "regression-checkpoint") > 0);
}

TEST_CASE("a failing gate loops back, then blocks at the attempt limit")
{
	engine_fixture fixture(1);
	odin_error err;

	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "blocked");
	CHECK(state.at("reason").get<std::string>().find("attempt limit") != std::string::npos);

	const json context = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));

	const std::vector<std::string> stages = stage_sequence(context);
	CHECK(count_of(stages, "implement") > 1);
	CHECK(count_of(stages, "quality-gate") > 1);
}

TEST_CASE("automatic staging is disabled by default")
{
	engine_fixture fixture;
	odin_error err;

	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));
	engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));

	const json context = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));

	const json &staging = context.at("artifacts").at("staging").at("artifacts");
	CHECK(staging.at("staged_files") == json::array());
	CHECK(staging.at("staging_manifest") == json::array({"README.md"}));
}

TEST_CASE("create_run lays out the durable state a gui polls")
{
	engine_fixture fixture;
	odin_error err;

	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	CHECK(fs::exists(run_dir / "task.json"));
	CHECK(fs::exists(run_dir / "context.json"));
	CHECK(fs::exists(run_dir / "state.json"));

	const json state = json_read(run_dir / "state.json", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("schema_version") == 1);
	CHECK(state.at("status") == "running");
	CHECK(state.at("workflow") == "feature");
	CHECK(state.at("current_stage") == "specify");
	CHECK(state.at("transitions") == 0);
	// isoformat(timespec="seconds") against a utc offset
	CHECK(state.at("created_at").get<std::string>().size() == 25);
	CHECK(state.at("created_at").get<std::string>().find("+00:00") != std::string::npos);

	const json context = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));
	// run_dir is stored relative to root while task_file is absolute
	CHECK(context.at("run_dir").get<std::string>().rfind(".odin", 0) == 0);
	CHECK(fs::path(context.at("task_file").get<std::string>()).is_absolute());
}

TEST_CASE("each executed stage leaves a zero padded event file")
{
	engine_fixture fixture;
	odin_error err;

	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));
	engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));

	CHECK(fs::exists(run_dir / "events" / "001-specify.json"));
	CHECK(fs::exists(run_dir / "events" / "008-stage.json"));

	const json first = json_read(run_dir / "events" / "001-specify.json", err);
	REQUIRE_FALSE(failed(err));
	CHECK(first.at("sequence") == 1);
	CHECK(first.at("attempt") == 1);
	CHECK(first.at("kind") == "agent");
	CHECK(first.at("result").at("status") == "approved");
	CHECK(first.at("metadata").at("model") == "fixture");
}

TEST_CASE("an unroutable model is a workflow error, not a blocked run")
{
	// a defect the run cannot describe itself: routing points at nothing
	engine_fixture fixture;
	fixture.machine.config.routing.clear();

	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	engine_run(fixture.machine, run_dir, "", err);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::workflow);
	CHECK(err.message == "no model route configured for agent 'analyst'");
}

TEST_CASE("an unconfigured gate is a workflow error")
{
	engine_fixture fixture;
	fixture.machine.config.gates.clear();

	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	engine_run(fixture.machine, run_dir, "", err);
	REQUIRE(failed(err));
	CHECK(err.message == "gate 'quality' is not configured");
}

TEST_CASE("an adapter that fails becomes a blocked handoff, not a crash")
{
	engine_fixture fixture;
	// point the adapter at a command that always fails
	fixture.machine.config.adapters["mock"].command = {"python", "-c",
													   "import sys; sys.stderr.write('nope'); "
													   "raise SystemExit(4)"};

	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "blocked");

	const json context = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));
	const json &first = context.at("history").at(0);
	CHECK(first.at("result").at("status") == "blocked");
	CHECK(first.at("result").at("summary") == "adapter 'mock' exited 4: nope");
	CHECK(first.at("metadata").at("adapter") == "mock");
}

TEST_CASE("the total transition budget stops a run that will not settle")
{
	engine_fixture fixture(1);
	fixture.machine.config.max_total_transitions = 3;

	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "blocked");
	CHECK(state.at("reason") == "maximum total transitions reached");
	CHECK(state.at("transitions") == 3);
}

TEST_CASE("a model override replaces routing for every agent stage")
{
	engine_fixture fixture;
	fixture.machine.config.models["other"] = fixture.machine.config.models.at("test");
	fixture.machine.config.models["other"].name = "other";
	fixture.machine.config.models["other"].model = "override-model";

	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));
	engine_run(fixture.machine, run_dir, "other", err);
	REQUIRE_FALSE(failed(err));

	const json first = json_read(run_dir / "events" / "001-specify.json", err);
	REQUIRE_FALSE(failed(err));
	CHECK(first.at("metadata").at("model") == "override-model");
}

TEST_CASE("a run resumes from its durable state")
{
	engine_fixture fixture;
	odin_error err;

	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	// run to completion, then run again: the state is terminal so nothing more
	// should happen and the history must not grow.
	const json first = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	const json context_before = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));

	const json second = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	const json context_after = json_read(run_dir / "context.json", err);
	REQUIRE_FALSE(failed(err));

	CHECK(second.at("status") == first.at("status"));
	CHECK(context_after.at("history").size() == context_before.at("history").size());
}
