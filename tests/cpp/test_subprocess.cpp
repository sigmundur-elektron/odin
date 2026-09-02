#include <doctest/doctest.h>

#include "adapter.h"
#include "atomic_file.h"
#include "subprocess.h"
#include "test_support.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

// the thing under test here is the process boundary itself, so there is nothing
// useful to mock and a real child has to exist. odin_test_child is that child:
// a directive-driven helper built from tests/cpp/test_child.cpp. it replaced
// `python -c "<snippet>"`, which quietly kept an interpreter mandatory for
// ctest long after the product stopped needing one.
static subprocess_options child(const std::vector<std::string> &directives)
{
	subprocess_options options;
	options.command = {ODIN_TEST_CHILD};
	options.command.insert(options.command.end(), directives.begin(), directives.end());
	return options;
}

TEST_CASE("subprocess_run captures stdout and the exit code")
{
	odin_error err;
	const subprocess_result result = subprocess_run(child({"out:hello"}), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.exit_code == 0);
	CHECK(result.stdout_text.find("hello") != std::string::npos);
}

TEST_CASE("a command that runs and fails is not an error")
{
	// the distinction harness/adapters.py makes by testing returncode rather
	// than passing check=True
	odin_error err;
	const subprocess_result result = subprocess_run(child({"exit:3"}), err);

	CHECK_FALSE(failed(err));
	CHECK(result.exit_code == 3);
}

TEST_CASE("stdout and stderr are captured separately")
{
	odin_error err;
	const subprocess_result result = subprocess_run(child({"out:out", "err:err"}), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text == "out");
	CHECK(result.stderr_text == "err");
}

TEST_CASE("merge_stderr folds stderr into stdout")
{
	subprocess_options options = child({"out:out", "err:err"});
	options.merge_stderr = true;

	odin_error err;
	const subprocess_result result = subprocess_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text.find("out") != std::string::npos);
	CHECK(result.stdout_text.find("err") != std::string::npos);
	CHECK(result.stderr_text.empty());
}

TEST_CASE("stdin is written and the child sees EOF")
{
	subprocess_options options = child({"cat"});
	options.input = "fed on stdin";

	odin_error err;
	const subprocess_result result = subprocess_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text == "fed on stdin");
}

TEST_CASE("a large stdin payload does not deadlock against a large stdout")
{
	// the failure mode this guards against: writing stdin to completion before
	// draining stdout, so both pipes fill and neither side can advance. the
	// payload is comfortably past the 64k pipe buffer on both platforms.
	std::string payload(512 * 1024, 'x');

	subprocess_options options = child({"cat"});
	options.input = payload;
	options.timeout_seconds = 60;

	odin_error err;
	const subprocess_result result = subprocess_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text.size() == payload.size());
}

TEST_CASE("a timeout is reported the way subprocess.TimeoutExpired reads")
{
	subprocess_options options = child({"sleep:30"});
	options.timeout_seconds = 1;

	const auto started = std::chrono::steady_clock::now();
	odin_error err;
	subprocess_run(options, err);
	const auto elapsed = std::chrono::steady_clock::now() - started;

	REQUIRE(failed(err));
	CHECK(err.message ==
		  "Command '" + python_list_repr(options.command) + "' timed out after 1 seconds");
	// it must actually stop waiting, not merely say so
	CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 15);
}

TEST_CASE("a timeout kills the whole process tree")
{
	// a cli agent launches the real agent binary as a grandchild, so killing
	// only the direct child would strand it. reproc uses TerminateProcess
	// alone; subprocess.cpp adds a job object to cover this.
	const temp_dir dir;
	const auto marker = dir.path / "grandchild-was-here.txt";

	// the child detaches a grandchild that waits, then writes the marker. if
	// the grandchild survives the kill it will create the file.
	subprocess_options options =
	  child({"spawn:3:" + file_path_utf8(marker), "sleep:30"});
	options.timeout_seconds = 1;

	odin_error err;
	subprocess_run(options, err);
	REQUIRE(failed(err));

	// outlive the grandchild's own sleep before checking
	std::this_thread::sleep_for(std::chrono::seconds(5));
	CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("the working directory and extra environment reach the child")
{
	const temp_dir dir;

	subprocess_options options = child({"cwd", "out:|", "env:ODIN_X"});
	options.working_directory = dir.path;
	options.environment["ODIN_X"] = "from-config";

	odin_error err;
	const subprocess_result result = subprocess_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text.find("from-config") != std::string::npos);
	// the inherited environment survives alongside the overlay
	CHECK(result.stdout_text.find(dir.path.filename().string()) != std::string::npos);
}

TEST_CASE("the child environment keeps operations but excludes unrelated secrets")
{
#ifdef _WIN32
	_putenv_s("ODIN_SECRET_CANARY", "do-not-leak");
#else
	setenv("ODIN_SECRET_CANARY", "do-not-leak", 1);
#endif
	subprocess_options options = child({"has:PATH", "out:|", "has:ODIN_SECRET_CANARY"});
	options.environment["ODIN_X"] = "1";

	odin_error err;
	const subprocess_result result = subprocess_run(options, err);
	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text == "True|False");
}

TEST_CASE("an explicitly inherited environment variable reaches only that child")
{
#ifdef _WIN32
	_putenv_s("ODIN_EXPLICIT_CANARY", "imported");
#else
	setenv("ODIN_EXPLICIT_CANARY", "imported", 1);
#endif
	subprocess_options options = child({"envor:ODIN_EXPLICIT_CANARY=missing"});
	options.inherit_environment.push_back("ODIN_EXPLICIT_CANARY");
	odin_error err;
	const subprocess_result result = subprocess_run(options, err);
	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text == "imported");
}

TEST_CASE("a command that cannot be launched is an error, not an exit code")
{
	subprocess_options options;
	options.command = {"odin-no-such-executable-anywhere"};

	odin_error err;
	subprocess_run(options, err);
	CHECK(failed(err));
}

TEST_CASE("python_list_repr matches python's repr of a list of strings")
{
	CHECK(python_list_repr({"python", "-m", "unittest"}) == "['python', '-m', 'unittest']");
	CHECK(python_list_repr({}) == "[]");
	CHECK(python_list_repr({"a"}) == "['a']");
	// python switches to double quotes when the value contains a single quote
	CHECK(python_list_repr({"it's"}) == "[\"it's\"]");
}

// ---------------------------------------------------------------- adapter

static project_config adapter_config(const std::filesystem::path &root)
{
	project_config config;
	config.root = root;
	return config;
}

static model_profile mock_profile()
{
	model_profile profile;
	profile.name = "mock";
	profile.adapter = "mock";
	profile.model = "deterministic-contract-fixture";
	profile.parameter_billions = json(0);
	return profile;
}

static command_spec child_spec(const std::vector<std::string> &directives)
{
	command_spec spec;
	spec.command = {ODIN_TEST_CHILD};
	spec.command.insert(spec.command.end(), directives.begin(), directives.end());
	return spec;
}

TEST_CASE("adapter_run drives the deterministic mock agent")
{
	command_spec spec = child_spec({"mock", "--model", "{model}"});
	spec.timeout_seconds = 30;

	json request;
	request["contract"] = "handoff/v1";
	request["agent"] = json{{"id", "reviewer"}};
	request["stage"] = json{{"id", "review"}};
	request["task"] = json{{"id", "demo"}};

	odin_error err;
	const adapter_result result =
	  adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.response.at("status") == "approved");
	CHECK(result.response.at("summary") == "reviewer approved using deterministic-contract-fixture");
	CHECK(result.metadata.at("exit_code") == 0);
	CHECK(result.metadata.at("model") == "deterministic-contract-fixture");
	CHECK(result.metadata.at("model_profile") == "mock");
	// the toml integer must not have become a float on the way through
	CHECK(result.metadata.at("parameter_billions").is_number_integer());
	CHECK(result.metadata.at("duration_seconds").is_number_float());
}

TEST_CASE("adapter environment excludes parent secrets and keeps explicit values")
{
#ifdef _WIN32
	_putenv_s("OPENAI_API_KEY", "parent-secret");
#else
	setenv("OPENAI_API_KEY", "parent-secret", 1);
#endif
	command_spec spec = child_spec({"handoff", "artenv:secret=OPENAI_API_KEY",
									"artenv:configured=ODIN_CONFIGURED",
									"artenv:root=ODIN_PROJECT_ROOT"});
	spec.environment["ODIN_CONFIGURED"] = "yes";
	json request;
	odin_error err;
	const project_config config = adapter_config(ODIN_REPO_ROOT);
	const adapter_result result = adapter_run(spec, mock_profile(), request, config, err);
	REQUIRE_FALSE(failed(err));
	CHECK(result.response.at("artifacts").at("secret").is_null());
	CHECK(result.response.at("artifacts").at("configured") == "yes");
	CHECK(result.response.at("artifacts").at("root") == file_path_utf8(config.root));

	spec.inherit_environment.push_back("OPENAI_API_KEY");
	const adapter_result imported = adapter_run(spec, mock_profile(), request, config, err);
	REQUIRE_FALSE(failed(err));
	CHECK(imported.response.at("artifacts").at("secret") == "[REDACTED]");
}

TEST_CASE("adapter output redacts explicitly inherited secrets")
{
#ifdef _WIN32
	_putenv_s("OPENAI_API_KEY", "super-secret-value");
#else
	setenv("OPENAI_API_KEY", "super-secret-value", 1);
#endif
	command_spec spec = child_spec({"errenv:OPENAI_API_KEY", "handoff"});
	spec.inherit_environment.push_back("OPENAI_API_KEY");
	json request;
	odin_error err;
	const adapter_result result =
	  adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
	REQUIRE_FALSE(failed(err));
	CHECK(result.metadata.at("stderr") == "[REDACTED]");
}

TEST_CASE("adapter JSON redaction handles escaped secret characters")
{
#ifdef _WIN32
	_putenv_s("ODIN_REVIEW_SECRET", "quote-\"-slash-\\-secret");
#else
	setenv("ODIN_REVIEW_SECRET", "quote-\"-slash-\\-secret", 1);
#endif
	command_spec spec = child_spec({"handoff", "artenv:value=ODIN_REVIEW_SECRET"});
	spec.inherit_environment.push_back("ODIN_REVIEW_SECRET");
	json request;
	odin_error err;
	const adapter_result result =
	  adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
	REQUIRE_FALSE(failed(err));
	CHECK(result.response.at("artifacts").at("value") == "[REDACTED]");
}

TEST_CASE("adapter diagnostics redact ascii escaped unicode secrets")
{
	const std::string secret = "caf\xc3\xa9-secret";
#ifdef _WIN32
	_putenv_s("ODIN_UNICODE_SECRET", secret.c_str());
#else
	setenv("ODIN_UNICODE_SECRET", secret.c_str(), 1);
#endif
	// the child writes the secret as an ensure_ascii JSON string, so the bytes
	// on stderr are caf\u00e9-secret rather than the raw value. redaction has
	// to catch that escaped form too.
	command_spec spec = child_spec({"errjson:ODIN_UNICODE_SECRET", "exit:1"});
	spec.inherit_environment.push_back("ODIN_UNICODE_SECRET");
	json request;
	odin_error err;
	adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
	REQUIRE(failed(err));
	CHECK(err.message.find(secret) == std::string::npos);
	CHECK(err.message.find("\\u00e9") == std::string::npos);
	CHECK(err.message.find("[REDACTED]") != std::string::npos);
}

TEST_CASE("{model} is substituted into the adapter command")
{
	// asserted twice on purpose: once on the recorded command, and once on an
	// artifact the child built from the substituted value, which proves the
	// expansion actually reached the process rather than only the metadata.
	command_spec spec = child_spec({"handoff", "art:model={model}"});

	json request;
	odin_error err;
	const adapter_result result =
	  adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.metadata.at("command").at(2) == "art:model=deterministic-contract-fixture");
	CHECK(result.response.at("artifacts").at("model") == "deterministic-contract-fixture");
}

TEST_CASE("a brace in an adapter command is left alone")
{
	// str.format would raise on this; plain substitution must not
	command_spec spec = child_spec({"out:{\"status\": 1}"});

	json request;
	odin_error err;
	adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);

	// it fails contract-wise later, but expansion itself must not have thrown
	CHECK_FALSE(failed(err));
}

TEST_CASE("adapter failures carry the documented wording")
{
	json request;
	odin_error err;

	SUBCASE("nonzero exit")
	{
		command_spec spec = child_spec({"err:  boom  ", "exit:2"});

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.kind == error_kind::adapter);
		CHECK(err.message == "adapter 'mock' exited 2: boom");
	}

	SUBCASE("output that is not json")
	{
		command_spec spec = child_spec({"out:not json at all"});

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.message == "adapter 'mock' returned invalid JSON");
	}

	SUBCASE("json that is not an object")
	{
		command_spec spec = child_spec({"out:[1, 2]"});

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.message == "adapter 'mock' must return a JSON object");
	}

	SUBCASE("a timeout")
	{
		command_spec spec = child_spec({"sleep:30"});
		spec.timeout_seconds = 1;

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.message.rfind("adapter 'mock' failed to run: Command '", 0) == 0);
		CHECK(err.message.find("timed out after 1 seconds") != std::string::npos);
	}
}

TEST_CASE("a built-in adapter kind runs in process")
{
	// the deterministic mock used to be a Python fixture script spawned as a
	// child, which is why the checked-in odin.toml needed an interpreter to run
	// its own workflow at all.
	command_spec spec;
	spec.type = "mock";

	json request;
	request["agent"] = json{{"id", "implementer"}};

	odin_error err;
	const adapter_result result =
	  adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.response.at("status") == "approved");
	CHECK(result.response.at("summary") == "implementer approved using deterministic-contract-fixture");
	CHECK(result.response.at("artifacts").at("changed_files").at(0) == "README.md");

	// no process was started, but the metadata shape still matches a spawned
	// adapter so nothing downstream has to branch on the kind
	CHECK(result.metadata.at("command").is_array());
	CHECK(result.metadata.at("command").empty());
	CHECK(result.metadata.at("exit_code") == 0);
	CHECK(result.metadata.at("model") == "deterministic-contract-fixture");
	CHECK(result.metadata.at("duration_seconds").is_number_float());
}

TEST_CASE("the built-in mock answers every workflow role")
{
	json request;
	odin_error err;

	for (const char *agent : {"analyst", "reproducer", "implementer", "verifier", "finalizer",
							  "reviewer"})
	{
		CAPTURE(agent);
		command_spec spec;
		spec.type = "mock";
		request["agent"] = json{{"id", agent}};
		request["task"] = json{{"request", "do the thing"}};

		const adapter_result result =
		  adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE_FALSE(failed(err));
		CHECK(result.response.at("status") == "approved");
		CHECK(result.response.at("artifacts").is_object());
	}
}