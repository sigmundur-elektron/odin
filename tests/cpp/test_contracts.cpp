#include <doctest/doctest.h>

#include "atomic_file.h"
#include "contracts.h"
#include "definitions.h"
#include "json_io.h"
#include "sidecar.h"
#include "test_support.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;

// the repository root, where scripts/contract_service.py and harness/ live
static fs::path repo_root() { return fs::path(ODIN_REPO_ROOT); }

// a sidecar that stops itself, so a failed assertion cannot leave a python
// process behind.
struct scoped_sidecar
{
	sidecar service;

	scoped_sidecar()
	{
		sidecar_configure(service, repo_root(), "");
	}
	~scoped_sidecar() { sidecar_stop(service); }

	scoped_sidecar(const scoped_sidecar &) = delete;
	scoped_sidecar &operator=(const scoped_sidecar &) = delete;
};

static json valid_handoff()
{
	json value;
	value["status"] = "approved";
	value["summary"] = "did the thing";
	value["artifacts"] = json::object();
	value["findings"] = json::array();
	return value;
}

TEST_CASE("the sidecar answers a ping")
{
	scoped_sidecar host;

	json request;
	request["op"] = "ping";

	odin_error err;
	const json reply = sidecar_call(host.service, request, err);
	REQUIRE_FALSE(failed(err));
	CHECK(reply.at("ok") == true);
}

TEST_CASE("the sidecar is started lazily")
{
	scoped_sidecar host;
	// configured but never called: no child yet
	CHECK(host.service.child == nullptr);

	json request;
	request["op"] = "ping";

	odin_error err;
	sidecar_call(host.service, request, err);
	REQUIRE_FALSE(failed(err));
	CHECK(host.service.child != nullptr);
}

TEST_CASE("the sidecar serves many requests over one child")
{
	scoped_sidecar host;

	odin_error err;
	for (int i = 0; i < 25; ++i)
	{
		json request;
		request["op"] = "ping";
		const json reply = sidecar_call(host.service, request, err);
		REQUIRE_FALSE(failed(err));
		CHECK(reply.at("ok") == true);
	}
	CHECK(host.service.restarts == 0);
}

TEST_CASE("contract_validate accepts a well formed handoff")
{
	scoped_sidecar host;

	odin_error err;
	contract_validate(host.service, valid_handoff(), "handoff", "stage 'review' output", err);
	CHECK_FALSE(failed(err));
}

TEST_CASE("contract_validate reports harness/contracts.py's message verbatim")
{
	scoped_sidecar host;

	json broken = valid_handoff();
	broken["status"] = "ok";
	broken["summary"] = "";

	odin_error err;
	contract_validate(host.service, broken, "handoff", "stage 'review' output", err);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::contract);
	CHECK(err.message ==
		  "stage 'review' output violates handoff: "
		  "status: 'ok' is not one of ['approved', 'revision', 'blocked']; "
		  "summary: '' should be non-empty");
}

TEST_CASE("contract_validate rejects an unknown contract")
{
	scoped_sidecar host;

	odin_error err;
	contract_validate(host.service, json::object(), "nonexistent", "value", err);
	REQUIRE(failed(err));
	CHECK(err.message == "unknown contract: nonexistent");
}

TEST_CASE("the sidecar restarts once after its child dies")
{
	scoped_sidecar host;

	odin_error err;
	json request;
	request["op"] = "ping";
	sidecar_call(host.service, request, err);
	REQUIRE_FALSE(failed(err));
	REQUIRE(host.service.child != nullptr);

	// simulate a crash: kill the child out from under the client
	host.service.child->kill();
	host.service.child->wait(reproc::milliseconds(2000));

	json again;
	again["op"] = "ping";
	const json reply = sidecar_call(host.service, again, err);
	CHECK_FALSE(failed(err));
	CHECK(reply.at("ok") == true);
	CHECK(host.service.restarts == 1);
}

TEST_CASE("every bundled definition loads and validates")
{
	scoped_sidecar host;
	definitions defs;
	definitions_configure(defs, host.service, repo_root() / "harness");

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
	scoped_sidecar host;
	definitions defs;
	definitions_configure(defs, host.service, repo_root() / "harness");

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
	scoped_sidecar host;
	definitions defs;
	definitions_configure(defs, host.service, repo_root() / "harness");

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

	scoped_sidecar host;
	definitions defs;
	definitions_configure(defs, host.service, dir.path);

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

	scoped_sidecar host;
	definitions defs;
	definitions_configure(defs, host.service, dir.path);

	odin_error err;
	CHECK(definitions_load_agent(defs, "broken", err) == nullptr);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::contract);
	CHECK(err.message.find("violates agent:") != std::string::npos);
}
