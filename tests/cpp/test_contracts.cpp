#include <doctest/doctest.h>

#include "atomic_file.h"
#include "contracts.h"
#include "definitions.h"
#include "json_io.h"
#include "test_support.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// the repository root, where harness/ lives
static fs::path repo_root() { return fs::path(ODIN_REPO_ROOT); }

// contracts used to be a client for a long-lived Python child, so these tests
// needed a fixture that killed it on the way out. validation is native now: the
// struct owns nothing but a path and a cache, and there is nothing to tear down.
static contracts bundled_contracts()
{
	contracts service;
	contracts_configure(service, repo_root() / "harness" / "schemas");
	return service;
}

static json valid_handoff()
{
	json value;
	value["status"] = "approved";
	value["summary"] = "did the thing";
	value["artifacts"] = json::object();
	value["findings"] = json::array();
	return value;
}

TEST_CASE("contract_validate accepts a well formed handoff")
{
	contracts service = bundled_contracts();

	odin_error err;
	contract_validate(service, valid_handoff(), "handoff", "stage 'review' output", err);
	CHECK_FALSE(failed(err));
}

TEST_CASE("contract_validate names the subject, the contract and every violation")
{
	contracts service = bundled_contracts();

	json broken = valid_handoff();
	broken["status"] = "ok";
	broken["summary"] = "";

	odin_error err;
	contract_validate(service, broken, "handoff", "stage 'review' output", err);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::contract);
	// this text reaches durable state through a blocked stage's summary, so the
	// shape is a product surface. the wording is Odin's own; it deliberately no
	// longer reproduces jsonschema's phrasing or Python's repr quoting.
	CHECK(err.message ==
		  "stage 'review' output violates handoff: "
		  "status: \"ok\" is not one of [\"approved\", \"revision\", \"blocked\"]; "
		  "summary: must be at least 1 character");
}

TEST_CASE("violations are reported parent before child, in key order")
{
	contracts service = bundled_contracts();

	// two failures at different depths in one instance
	json broken = valid_handoff();
	broken.erase("artifacts");
	broken["findings"] = json::array({7});

	odin_error err;
	contract_validate(service, broken, "handoff", "value", err);
	REQUIRE(failed(err));
	CHECK(err.message == "value violates handoff: "
						 "<root>: required property 'artifacts' is missing; "
						 "findings/0: expected string, found integer");
}

TEST_CASE("contract_validate rejects an unknown contract")
{
	contracts service = bundled_contracts();

	odin_error err;
	contract_validate(service, json::object(), "nonexistent", "value", err);
	REQUIRE(failed(err));
	CHECK(err.message == "unknown contract: nonexistent");
}

TEST_CASE("a schema is parsed once and reused")
{
	contracts service = bundled_contracts();
	CHECK(service.cache.empty());

	odin_error err;
	contract_validate(service, valid_handoff(), "handoff", "value", err);
	REQUIRE_FALSE(failed(err));
	contract_validate(service, valid_handoff(), "handoff", "value", err);
	REQUIRE_FALSE(failed(err));

	CHECK(service.cache.size() == 1);
}

TEST_CASE("a schema using an unsupported keyword is refused, not partly applied")
{
	// the failure mode this guards against is silent under-validation: a
	// validator that skips the keyword it does not know still answers "valid".
	const temp_dir dir;
	temp_write(dir.path / "custom.schema.json",
			   R"({"type": "object", "properties": {"id": {"type": "string", "format": "uuid"}}})");

	contracts service;
	contracts_configure(service, dir.path);

	odin_error err;
	contract_validate(service, json::object(), "custom", "value", err);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::contract);
	CHECK(err.message.find("unsupported keyword 'format'") != std::string::npos);
}

TEST_CASE("every bundled definition loads and validates")
{
	contracts service = bundled_contracts();
	definitions defs;
	definitions_configure(defs, service, repo_root() / "harness");

	odin_error err;

	SUBCASE("agents")
	{
		for (const char *name : {"analyst", "finalizer", "implementer",
								 "reproducer", "reviewer", "verifier"})
		{
			CAPTURE(name);
			const json *agent = definitions_load_agent(defs, name, err);
			REQUIRE_FALSE(failed(err));
			REQUIRE(agent != nullptr);
			CHECK(agent->at("id") == name);
		}
	}

	SUBCASE("skills")
	{
		for (const char *name : {"bug-reproduction", "cpp-change", "finalization",
								 "review", "specification", "verification"})
		{
			CAPTURE(name);
			const json *skill = definitions_load_skill(defs, name, err);
			REQUIRE_FALSE(failed(err));
			REQUIRE(skill != nullptr);
		}
	}

	SUBCASE("workflows")
	{
		for (const char *name : {"feature", "bugfix"})
		{
			CAPTURE(name);
			const json *workflow = definitions_load_workflow(defs, name, err);
			REQUIRE_FALSE(failed(err));
			REQUIRE(workflow != nullptr);
			CHECK(workflow->at("stages").is_array());
		}
	}

	SUBCASE("templates")
	{
		for (const char *name : {"feature", "bugfix"})
		{
			CAPTURE(name);
			const json *task = definitions_load_template(defs, name, err);
			REQUIRE_FALSE(failed(err));
			REQUIRE(task != nullptr);
		}
	}
}

TEST_CASE("a definition is read and validated only once")
{
	contracts service = bundled_contracts();
	definitions defs;
	definitions_configure(defs, service, repo_root() / "harness");

	odin_error err;
	const json *first = definitions_load_agent(defs, "analyst", err);
	REQUIRE_FALSE(failed(err));
	const json *second = definitions_load_agent(defs, "analyst", err);
	REQUIRE_FALSE(failed(err));

	// the same cached object, not merely an equal one
	CHECK(first == second);
	CHECK(defs.cache.size() == 1);
}

TEST_CASE("a missing definition is reported as a workflow error")
{
	contracts service = bundled_contracts();
	definitions defs;
	definitions_configure(defs, service, repo_root() / "harness");

	odin_error err;
	CHECK(definitions_load_agent(defs, "absent", err) == nullptr);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::workflow);
	CHECK(err.message.rfind("file not found: ", 0) == 0);
}

TEST_CASE("a definition whose id disagrees with its filename is rejected")
{
	const temp_dir dir;
	const auto path = dir.path / "agents" / "wrong-name.json";

	json agent;
	agent["id"] = "other-name";
	agent["purpose"] = "a fixture";
	agent["reads"] = json::array();
	agent["writes"] = json::array();
	agent["skills"] = json::array();
	agent["rules"] = json::array();
	temp_write(path, agent.dump());

	contracts service = bundled_contracts();
	definitions defs;
	definitions_configure(defs, service, dir.path);

	odin_error err;
	CHECK(definitions_load_agent(defs, "wrong-name", err) == nullptr);
	REQUIRE(failed(err));
	CHECK(err.message == file_path_utf8(path) + ": id 'other-name' does not match filename 'wrong-name'");
}

TEST_CASE("a definition that violates its schema is rejected before the id check")
{
	const temp_dir dir;
	const auto path = dir.path / "agents" / "broken.json";
	temp_write(path, R"({"id": "broken", "purpose": ""})");

	contracts service = bundled_contracts();
	definitions defs;
	definitions_configure(defs, service, dir.path);

	odin_error err;
	CHECK(definitions_load_agent(defs, "broken", err) == nullptr);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::contract);
	CHECK(err.message.find("violates agent:") != std::string::npos);
}
