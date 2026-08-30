#include <doctest/doctest.h>

#include "adapter.h"
#include "atomic_file.h"
#include "process.h"
#include "test_support.h"

#include <chrono>
#include <string>
#include <thread>

// a small python program, run as a child. the existing python tests build their
// fixtures the same way (tests/test_adapters.py), because the thing under test
// is the process boundary itself - there is nothing useful to mock.
static process_options python_snippet(const std::string &source)
{
	process_options options;
	options.command = {"python", "-c", source};
	return options;
}

TEST_CASE("process_run captures stdout and the exit code")
{
	odin_error err;
	const process_result result =
	  process_run(python_snippet("print('hello')"), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.exit_code == 0);
	CHECK(result.stdout_text.find("hello") != std::string::npos);
}

TEST_CASE("a command that runs and fails is not an error")
{
	// the distinction harness/adapters.py makes by testing returncode rather
	// than passing check=True
	odin_error err;
	const process_result result = process_run(python_snippet("raise SystemExit(3)"), err);

	CHECK_FALSE(failed(err));
	CHECK(result.exit_code == 3);
}

TEST_CASE("stdout and stderr are captured separately")
{
	odin_error err;
	const process_result result = process_run(
	  python_snippet("import sys; sys.stdout.write('out'); sys.stderr.write('err')"), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text == "out");
	CHECK(result.stderr_text == "err");
}

TEST_CASE("merge_stderr folds stderr into stdout")
{
	process_options options =
	  python_snippet("import sys; sys.stdout.write('out'); sys.stdout.flush(); "
					 "sys.stderr.write('err')");
	options.merge_stderr = true;

	odin_error err;
	const process_result result = process_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text.find("out") != std::string::npos);
	CHECK(result.stdout_text.find("err") != std::string::npos);
	CHECK(result.stderr_text.empty());
}

TEST_CASE("stdin is written and the child sees EOF")
{
	process_options options = python_snippet("import sys; sys.stdout.write(sys.stdin.read())");
	options.input = "fed on stdin";

	odin_error err;
	const process_result result = process_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text == "fed on stdin");
}

TEST_CASE("a large stdin payload does not deadlock against a large stdout")
{
	// the failure mode this guards against: writing stdin to completion before
	// draining stdout, so both pipes fill and neither side can advance. the
	// payload is comfortably past the 64k pipe buffer on both platforms.
	std::string payload(512 * 1024, 'x');

	process_options options =
	  python_snippet("import sys; data = sys.stdin.read(); sys.stdout.write(data)");
	options.input = payload;
	options.timeout_seconds = 60;

	odin_error err;
	const process_result result = process_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text.size() == payload.size());
}

TEST_CASE("a timeout is reported the way subprocess.TimeoutExpired reads")
{
	process_options options = python_snippet("import time; time.sleep(30)");
	options.timeout_seconds = 1;

	const auto started = std::chrono::steady_clock::now();
	odin_error err;
	process_run(options, err);
	const auto elapsed = std::chrono::steady_clock::now() - started;

	REQUIRE(failed(err));
	CHECK(err.message == "Command '['python', '-c', 'import time; time.sleep(30)']' "
						 "timed out after 1 seconds");
	// it must actually stop waiting, not merely say so
	CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 15);
}

TEST_CASE("a timeout kills the whole process tree")
{
	// adapters/cli_agent.py launches the real agent CLI as a grandchild, so
	// killing only the direct child would strand it. reproc uses
	// TerminateProcess alone; process.cpp adds a job object to cover this.
	const temp_dir dir;
	const auto marker = dir.path / "grandchild-was-here.txt";

	// the parent spawns a grandchild that waits, then writes the marker. if the
	// grandchild survives the kill it will create the file.
	const std::string grandchild =
	  "import sys,time; time.sleep(3); open(r'" + file_path_utf8(marker) + "','w').close()";
	const std::string parent = "import subprocess,sys,time; "
							   "subprocess.Popen([sys.executable,'-c',r'''" +
							   grandchild + "''']); time.sleep(30)";

	process_options options = python_snippet(parent);
	options.timeout_seconds = 1;

	odin_error err;
	process_run(options, err);
	REQUIRE(failed(err));

	// outlive the grandchild's own sleep before checking
	std::this_thread::sleep_for(std::chrono::seconds(5));
	CHECK_FALSE(std::filesystem::exists(marker));
}

TEST_CASE("the working directory and extra environment reach the child")
{
	const temp_dir dir;

	process_options options =
	  python_snippet("import os,sys; sys.stdout.write(os.getcwd() + '|' + os.environ['ODIN_X'])");
	options.working_directory = dir.path;
	options.environment["ODIN_X"] = "from-config";

	odin_error err;
	const process_result result = process_run(options, err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text.find("from-config") != std::string::npos);
	// the inherited environment survives alongside the overlay
	CHECK(result.stdout_text.find(dir.path.filename().string()) != std::string::npos);
}

TEST_CASE("the inherited environment is extended, not replaced")
{
	process_options options =
	  python_snippet("import os,sys; sys.stdout.write(str(len(os.environ) > 3))");
	options.environment["ODIN_X"] = "1";

	odin_error err;
	const process_result result = process_run(options, err);
	REQUIRE_FALSE(failed(err));
	CHECK(result.stdout_text == "True");
}

TEST_CASE("a command that cannot be launched is an error, not an exit code")
{
	process_options options;
	options.command = {"odin-no-such-executable-anywhere"};

	odin_error err;
	process_run(options, err);
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

TEST_CASE("adapter_run drives the bundled mock agent")
{
	command_spec spec;
	spec.command = {"python", "scripts/mock_agent.py", "--model", "{model}"};
	spec.timeout_seconds = 30;

	json request;
	request["contract"] = "handoff/v1";
	request["agent"] = json{ { "id", "reviewer" } };
	request["stage"] = json{ { "id", "review" } };
	request["task"] = json{ { "id", "demo" } };

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

TEST_CASE("{model} is substituted into the adapter command")
{
	command_spec spec;
	spec.command = {"python", "-c", "import sys; sys.stdout.write('{}')", "{model}"};

	json request;
	odin_error err;
	const adapter_result result =
	  adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);

	REQUIRE_FALSE(failed(err));
	CHECK(result.metadata.at("command").at(3) == "deterministic-contract-fixture");
}

TEST_CASE("a brace in an adapter command is left alone")
{
	// str.format would raise on this; plain substitution must not
	command_spec spec;
	spec.command = {"python", "-c", "import sys; sys.stdout.write('{\"status\": 1}')"};

	json request;
	odin_error err;
	adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);

	// it fails contract-wise later, but expansion itself must not have thrown
	CHECK_FALSE(failed(err));
}

TEST_CASE("adapter failures carry harness/adapters.py's wording")
{
	json request;
	odin_error err;

	SUBCASE("nonzero exit")
	{
		command_spec spec;
		spec.command = {"python", "-c",
						"import sys; sys.stderr.write('  boom  '); raise SystemExit(2)"};

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.kind == error_kind::adapter);
		CHECK(err.message == "adapter 'mock' exited 2: boom");
	}

	SUBCASE("output that is not json")
	{
		command_spec spec;
		spec.command = {"python", "-c", "print('not json at all')"};

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.message == "adapter 'mock' returned invalid JSON");
	}

	SUBCASE("json that is not an object")
	{
		command_spec spec;
		spec.command = {"python", "-c", "print('[1, 2]')"};

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.message == "adapter 'mock' must return a JSON object");
	}

	SUBCASE("a timeout")
	{
		command_spec spec;
		spec.command = {"python", "-c", "import time; time.sleep(30)"};
		spec.timeout_seconds = 1;

		adapter_run(spec, mock_profile(), request, adapter_config(ODIN_REPO_ROOT), err);
		REQUIRE(failed(err));
		CHECK(err.message.rfind("adapter 'mock' failed to run: Command '", 0) == 0);
		CHECK(err.message.find("timed out after 1 seconds") != std::string::npos);
	}
}
