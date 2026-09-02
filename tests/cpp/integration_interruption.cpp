// Interruption and recovery smoke test.
//
// The durable-state contract is the one thing a crash can silently violate, so
// this drives a real interruption: it starts a run whose first adapter hangs,
// kills Odin mid-attempt, kills the stranded child, and then checks that a
// plain resume refuses to replay uncertain external work while an explicit
// retry does.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <reproc++/reproc.hpp>

#include "atomic_file.h"
#include "json_io.h"
#include "subprocess.h"

namespace fs = std::filesystem;

namespace
{

int failures = 0;

void complain(const std::string &message)
{
	std::fprintf(stderr, "FAIL: %s\n", message.c_str());
	++failures;
}

void require(bool condition, const std::string &message)
{
	if (!condition)
		complain(message);
}

void write_text(const fs::path &path, const std::string &contents)
{
	fs::create_directories(path.parent_path());
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

std::string read_text(const fs::path &path)
{
	std::ifstream stream(path, std::ios::binary);
	return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

subprocess_result run(const std::vector<std::string> &command, const fs::path &working_directory)
{
	subprocess_options options;
	options.command = command;
	options.working_directory = working_directory;
	options.timeout_seconds = 60;
	odin_error err;
	return subprocess_run(options, err);
}

// poll rather than sleep a fixed amount: the wait is for an external process to
// reach a specific point, and a fixed sleep would be both slower and flakier
bool wait_for_file(const fs::path &path, int seconds)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	while (std::chrono::steady_clock::now() < deadline)
	{
		if (fs::exists(path))
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	return false;
}

std::string option(int argc, char **argv, const std::string &name, const std::string &fallback)
{
	for (int at = 1; at + 1 < argc; ++at)
	{
		if (name == argv[at])
			return argv[at + 1];
	}
	return fallback;
}

} // namespace

int main(int argc, char **argv)
{
	const std::string odin = option(argc, argv, "--odin", "");
	const std::string runtime_root = option(argc, argv, "--runtime-root", "");
	const std::string helper = option(argc, argv, "--helper", "");

	std::mt19937 engine{std::random_device{}()};
	const fs::path project =
	  fs::temp_directory_path() /
	  ("odin interruption test " +
	   std::to_string(std::uniform_int_distribution<int>(0, 0xffffff)(engine)));
	fs::create_directories(project);

	write_text(project / "task.json",
			   R"({"id":"interrupt-smoke","kind":"feature","title":"Interruption smoke",)"
			   R"("request":"Exercise interruption recovery."})");

	// the adapter counts its invocations, records its pid, announces itself,
	// and then hangs on the FIRST call only - so the retry can complete
	write_text(project / "odin.toml",
			   "[harness]\nstate_dir = \".odin/runs\"\nmax_total_transitions = 30\n"
			   "[git]\nstage_on_success = false\n"
			   "[adapters.test]\ncommand = [" +
				 json(helper).dump() +
				 ", \"count:invocations.txt\", \"pid:agent.pid\", \"touch:first-started\", "
				 "\"sleep-if-first:120\", \"handoff\", " +
				 json(std::string(R"(artjson:changed_files=["task.json"])")).dump() +
				 "]\ntimeout_seconds = 180\n"
				 "[models.test]\nadapter = \"test\"\nmodel = \"interrupt\"\n"
				 "[routing]\ndefault = \"test\"\n"
				 "[gates.quality]\ncommand = [" +
				 json(helper).dump() + ", \"exit:0\"]\ntimeout_seconds = 30\n");

	const std::string config = file_path_utf8(project / "odin.toml");

	reproc::options options;
	const std::string working_directory = file_path_utf8(project);
	options.working_directory = working_directory.c_str();
	options.redirect.out.type = reproc::redirect::pipe;
	options.redirect.err.type = reproc::redirect::pipe;
	std::map<std::string, std::string> environment;
	odin_error environment_error;
	subprocess_environment_build({}, {{"ODIN_RUNTIME_ROOT", runtime_root}}, environment,
								 environment_error);
	options.env.behavior = reproc::env::empty;
	options.env.extra = environment;

	const std::vector<std::string> start_command = {
	  odin, "--config", config, "start", file_path_utf8(project / "task.json")};

	reproc::process starter;
	const std::error_code start_code = starter.start(start_command, options);
	require(!start_code, "could not start odin: " + start_code.message());

	if (!start_code)
	{
		if (!wait_for_file(project / "first-started", 20))
		{
			complain("timed out waiting for the first adapter invocation");
		}
		else
		{
			// the run directory exists as soon as the attempt is journaled
			fs::path run_dir;
			for (const fs::directory_entry &entry :
				 fs::directory_iterator(project / ".odin" / "runs"))
			{
				if (entry.is_directory())
					run_dir = entry.path();
			}
			require(!run_dir.empty(), "no run directory was created");

			odin_error err;
			const json state = json_read(run_dir / "state.json", err);
			require(!failed(err) && state.contains("in_progress") &&
					  state.at("in_progress").value("stage", "") == "specify",
					"the active attempt was not persisted before the adapter ran");

			// a second process must refuse to touch a run that is executing
			const subprocess_result concurrent =
			  run({odin, "--config", config, "resume", file_path_utf8(run_dir)}, project);
			require(concurrent.exit_code == 2 &&
					  concurrent.stderr_text.find("already being executed") != std::string::npos,
					"a concurrent resume was not rejected");

			// kill odin, then the stranded adapter, simulating a hard crash
			starter.kill();
			starter.wait(reproc::milliseconds(10000));

			const std::string pid_text = read_text(project / "agent.pid");
			if (!pid_text.empty())
			{
				const std::vector<std::string> kill =
#ifdef _WIN32
				  {"taskkill", "/PID", pid_text, "/F"};
#else
				  {"kill", "-TERM", pid_text};
#endif
				run(kill, project);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(500));

			// plain resume must report uncertainty rather than silently
			// replaying work whose external effect is unknown
			const subprocess_result plain =
			  run({odin, "--config", config, "resume", file_path_utf8(run_dir)}, project);
			require(plain.exit_code == 2, "plain interrupted resume unexpectedly succeeded");
			const json blocked = json::parse(plain.stdout_text, nullptr, false);
			require(!blocked.is_discarded() &&
					  blocked.at("state").value("reason_code", "") == "outcome_uncertain",
					"the interrupted run was not marked uncertain");
			require(read_text(project / "invocations.txt") == "1",
					"plain resume reran uncertain external work");

			// acknowledging a possible duplicate effect is explicit
			const subprocess_result retried =
			  run({odin, "--config", config, "resume", file_path_utf8(run_dir),
				   "--retry-interrupted"},
				  project);
			require(retried.exit_code == 0, "explicit retry did not complete");
			const json finished = json::parse(retried.stdout_text, nullptr, false);
			require(!finished.is_discarded() &&
					  finished.at("state").value("status", "") == "complete",
					"explicit retry did not reach complete");
			require(read_text(project / "invocations.txt") != "1",
					"explicit retry did not execute a new attempt");
		}
	}

	starter.kill();
	starter.wait(reproc::milliseconds(5000));

	std::error_code ignored;
	fs::remove_all(project, ignored);

	if (failures == 0)
		std::printf("interruption recovery: ok\n");
	return failures == 0 ? 0 : 1;
}
