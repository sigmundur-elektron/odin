#include <doctest/doctest.h>

#include "atomic_file.h"
#include "config.h"
#include "contracts.h"
#include "definitions.h"
#include "engine.h"
#include "json_io.h"
#include "run_store.h"
#include "sidecar.h"
#include "subprocess.h"
#include "test_support.h"

#include <cstdlib>
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

static subprocess_result engine_git(const fs::path &root,
									const std::vector<std::string> &arguments,
									odin_error &out_error)
{
	subprocess_options options;
	options.command = {"git"};
	options.command.insert(options.command.end(), arguments.begin(), arguments.end());
	options.working_directory = root;
	options.merge_stderr = true;
	options.timeout_seconds = 30;
	return subprocess_run(options, out_error);
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

TEST_CASE("automatic staging uses literal validated paths")
{
	engine_fixture fixture;
	fixture.machine.config.stage_on_success = true;
	const fs::path literal = fixture.dir.path / "[ab].txt";
	temp_write(literal, "literal\n");
	temp_write(fixture.dir.path / "a.txt", "other\n");
	// The bundled mock reports README.md, so use that exact literal as the
	// bracketed filename through a small deterministic adapter.
	const std::string source =
	  "import json,sys; r=json.load(sys.stdin); a=r['agent']['id']; "
	  "p=['[ab].txt'] if a=='implementer' else []; "
	  "json.dump({'status':'approved','summary':'ok','artifacts':{'changed_files':p},'findings':[]},sys.stdout)";
	fixture.machine.config.adapters["mock"].command = {"python", "-c", source};

	odin_error err;
	REQUIRE(engine_git(fixture.dir.path, {"init", "--quiet"}, err).exit_code == 0);
	REQUIRE_FALSE(failed(err));
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));
	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "complete");

	const subprocess_result cached =
	  engine_git(fixture.dir.path, {"diff", "--cached", "--name-only"}, err);
	REQUIRE_FALSE(failed(err));
	CHECK(cached.stdout_text.find("[ab].txt") != std::string::npos);
	CHECK(cached.stdout_text.find("a.txt") == std::string::npos);
}

TEST_CASE("malformed changed files block staging without throwing")
{
	engine_fixture fixture;
	const std::string source =
	  "import json,sys; r=json.load(sys.stdin); a=r['agent']['id']; "
	  "p=[3] if a=='implementer' else []; "
	  "json.dump({'status':'approved','summary':'ok','artifacts':{'changed_files':p},'findings':[]},sys.stdout)";
	fixture.machine.config.adapters["mock"].command = {"python", "-c", source};

	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));
	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "blocked");
	const json context = json_read(run_dir / "context.json", err);
	const json &staging = context.at("artifacts").at("staging");
	CHECK(staging.at("summary") == "explicit changed_files artifact is invalid");
	CHECK(staging.at("findings").at(0) == "changed_files[0] must be a string");
}

TEST_CASE("a gate does not inherit unrelated provider secrets")
{
#ifdef _WIN32
	_putenv_s("OPENAI_API_KEY", "parent-secret");
#else
	setenv("OPENAI_API_KEY", "parent-secret", 1);
#endif
	engine_fixture fixture;
	fixture.machine.config.gates["quality"].command = {
	  "python", "-c",
	  "import os; print(os.environ.get('OPENAI_API_KEY','absent')+'|'+os.environ.get('ODIN_GATE','missing'))"};
	fixture.machine.config.gates["quality"].environment["ODIN_GATE"] = "configured";
	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));
	const json state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(state.at("status") == "complete");
	const json context = json_read(run_dir / "context.json", err);
	const std::string output = context.at("artifacts")
								 .at("gate_result")
								 .at("artifacts")
								 .at("gate")
								 .at("output");
	CHECK(output.find("absent|configured") != std::string::npos);
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
	CHECK(state.at("schema_version") == 2);
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

	std::vector<json> journal;
	REQUIRE(run_journal_load(run_dir, journal, err));
	CHECK(journal.size() == 16);
	CHECK(journal.front().at("type") == "stage_started");
	CHECK(journal.at(1).at("type") == "stage_completed");
	CHECK(journal.front().at("execution_id") == journal.at(1).at("execution_id"));
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

TEST_CASE("a held run lock prevents concurrent execution")
{
	engine_fixture fixture;
	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	run_lock lock;
	REQUIRE(run_lock_acquire(run_dir, lock, err));
	err = {};
	engine_run(fixture.machine, run_dir, "", err);
	REQUIRE(failed(err));
	CHECK(err.message.find("already being executed") != std::string::npos);

	odin_error read_error;
	const json state = json_read(run_dir / "state.json", read_error);
	REQUIRE_FALSE(failed(read_error));
	CHECK(state.at("stage_attempts").empty());
}

TEST_CASE("an unmatched stage start blocks until retry is explicitly acknowledged")
{
	engine_fixture fixture;
	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	json state = json_read(run_dir / "state.json", err);
	REQUIRE_FALSE(failed(err));
	const json started{{"journal_version", 1},
					   {"type", "stage_started"},
					   {"execution_id", "interrupted-execution"},
					   {"sequence", 1},
					   {"stage", "specify"},
					   {"kind", "agent"},
					   {"attempt", 1},
					   {"at", "2026-08-31T00:00:00+00:00"}};
	REQUIRE(run_journal_publish(run_dir, started, err));
	state["stage_attempts"]["specify"] = 1;
	state["in_progress"] = json{{"execution_id", "interrupted-execution"},
								{"sequence", 1},
								{"stage", "specify"},
								{"kind", "agent"},
								{"attempt", 1},
								{"started_at", "2026-08-31T00:00:00+00:00"}};
	json_write_atomic(run_dir / "state.json", state, err);
	REQUIRE_FALSE(failed(err));

	const json blocked = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(blocked.at("status") == "blocked");
	CHECK(blocked.at("reason_code") == "outcome_uncertain");
	CHECK(blocked.at("stage_attempts").at("specify") == 1);
	CHECK(json_read(run_dir / "context.json", err).at("history").empty());

	const json completed =
	  engine_run(fixture.machine, run_dir, engine_run_options{"", true}, err);
	REQUIRE_FALSE(failed(err));
	CHECK(completed.at("status") == "complete");
	CHECK(completed.at("stage_attempts").at("specify") == 2);
	const json resumed_again = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(resumed_again.at("status") == "complete");
	CHECK_FALSE(resumed_again.contains("reason_code"));
}

TEST_CASE("a committed journal completion repairs snapshots without rerunning its stage")
{
	engine_fixture fixture;
	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	const json started{{"journal_version", 1},
					   {"type", "stage_started"},
					   {"execution_id", "committed-execution"},
					   {"sequence", 1},
					   {"stage", "specify"},
					   {"kind", "agent"},
					   {"attempt", 1},
					   {"at", "2026-08-31T00:00:00+00:00"}};
	json completed = started;
	completed["type"] = "stage_completed";
	completed["at"] = "2026-08-31T00:00:01+00:00";
	completed["result"] = json{{"status", "approved"},
							   {"summary", "recovered specification"},
							   {"artifacts", json::object()},
							   {"findings", json::array()}};
	completed["metadata"] = json::object();
	REQUIRE(run_journal_publish(run_dir, started, err));
	REQUIRE(run_journal_publish(run_dir, completed, err));

	json state = json_read(run_dir / "state.json", err);
	state["stage_attempts"]["specify"] = 1;
	state["in_progress"] = json{{"execution_id", "committed-execution"},
								{"sequence", 1},
								{"stage", "specify"},
								{"kind", "agent"},
								{"attempt", 1},
								{"started_at", "2026-08-31T00:00:00+00:00"}};
	json_write_atomic(run_dir / "state.json", state, err);
	REQUIRE_FALSE(failed(err));

	const json final_state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(final_state.at("status") == "complete");
	CHECK(final_state.at("stage_attempts").at("specify") == 1);
	const json context = json_read(run_dir / "context.json", err);
	CHECK(context.at("history").at(0).at("result").at("summary") ==
		  "recovered specification");
}

TEST_CASE("a state snapshot ahead of context repairs history without a second transition")
{
	engine_fixture fixture;
	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	const json started{{"journal_version", 1},
					   {"type", "stage_started"},
					   {"execution_id", "state-ahead-execution"},
					   {"sequence", 1},
					   {"stage", "specify"},
					   {"kind", "agent"},
					   {"attempt", 1},
					   {"at", "2026-08-31T00:00:00+00:00"}};
	json completed = started;
	completed["type"] = "stage_completed";
	completed["at"] = "2026-08-31T00:00:01+00:00";
	completed["result"] = json{{"status", "approved"},
							   {"summary", "state already advanced"},
							   {"artifacts", json::object()},
							   {"findings", json::array()}};
	completed["metadata"] = json::object();
	REQUIRE(run_journal_publish(run_dir, started, err));
	REQUIRE(run_journal_publish(run_dir, completed, err));

	json state = json_read(run_dir / "state.json", err);
	state["current_stage"] = "review-spec";
	state["transitions"] = 1;
	state["stage_attempts"]["specify"] = 1;
	state["last_completed_execution_id"] = "state-ahead-execution";
	json_write_atomic(run_dir / "state.json", state, err);
	REQUIRE_FALSE(failed(err));

	const json final_state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(final_state.at("status") == "complete");
	CHECK(final_state.at("transitions") == 8);
	const json context = json_read(run_dir / "context.json", err);
	CHECK(context.at("history").at(0).at("result").at("summary") == "state already advanced");
	CHECK(context.at("history").size() == 8);
}

TEST_CASE("a legacy completion event is recovered without replacing or rerunning it")
{
	engine_fixture fixture;
	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	json state = json_read(run_dir / "state.json", err);
	state["schema_version"] = 1;
	json_write_atomic(run_dir / "state.json", state, err);
	const json legacy{{"sequence", 1},
					  {"stage", "specify"},
					  {"kind", "agent"},
					  {"attempt", 1},
					  {"at", "2026-08-31T00:00:00+00:00"},
					  {"result", json{{"status", "approved"},
									  {"summary", "legacy specification"},
									  {"artifacts", json::object()},
									  {"findings", json::array()}}},
					  {"metadata", json::object()}};
	json_write_atomic(run_dir / "events" / "001-specify.json", legacy, err);
	REQUIRE_FALSE(failed(err));

	const json final_state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(final_state.at("status") == "complete");
	CHECK(final_state.at("stage_attempts").at("specify") == 1);
	CHECK(json_read(run_dir / "context.json", err).at("history").at(0) == legacy);
}

TEST_CASE("a legacy context ahead of state advances without rerunning its completed stage")
{
	engine_fixture fixture;
	odin_error err;
	const fs::path run_dir =
	  engine_create_run(fixture.machine, feature_task(), fixture.dir.path / "feature.json", err);
	REQUIRE_FALSE(failed(err));

	json state = json_read(run_dir / "state.json", err);
	state["schema_version"] = 1;
	json_write_atomic(run_dir / "state.json", state, err);
	const json legacy{{"sequence", 1},
					  {"stage", "specify"},
					  {"kind", "agent"},
					  {"attempt", 1},
					  {"at", "2026-08-31T00:00:00+00:00"},
					  {"result", json{{"status", "approved"},
									  {"summary", "context already completed specification"},
									  {"artifacts", json::object()},
									  {"findings", json::array()}}},
					  {"metadata", json::object()}};
	json context = json_read(run_dir / "context.json", err);
	context["history"].push_back(legacy);
	context["artifacts"]["specification"] = legacy.at("result");
	json_write_atomic(run_dir / "context.json", context, err);
	json_write_atomic(run_dir / "events" / "001-specify.json", legacy, err);
	REQUIRE_FALSE(failed(err));

	const json final_state = engine_run(fixture.machine, run_dir, "", err);
	REQUIRE_FALSE(failed(err));
	CHECK(final_state.at("status") == "complete");
	CHECK(final_state.at("stage_attempts").at("specify") == 1);
	const json final_context = json_read(run_dir / "context.json", err);
	CHECK(final_context.at("history").at(0).at("result").at("summary") ==
		  "context already completed specification");
	CHECK(final_context.at("history").size() == 8);
}
