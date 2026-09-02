// Installed-layout smoke test.
//
// Everything else runs against the source tree; this one installs Odin the way
// a user would and drives it from a third, unrelated directory. Every path
// contains a space on purpose - quoting defects only appear here.
//
// A plain executable rather than a doctest suite: it takes the install
// parameters from CTest, and its value is one end-to-end pass or a clear
// failure, not a matrix of cases.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

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

// run a command and insist it succeeded, reporting both streams when it did not
bool run(const std::vector<std::string> &command, const fs::path &working_directory,
		 subprocess_result &out_result)
{
	subprocess_options options;
	options.command = command;
	options.working_directory = working_directory;
	options.timeout_seconds = 120;

	odin_error err;
	out_result = subprocess_run(options, err);
	if (failed(err))
	{
		complain(command_repr(command) + " could not run: " + err.message);
		return false;
	}
	if (out_result.exit_code != 0)
	{
		complain(command_repr(command) + " exited " + std::to_string(out_result.exit_code) +
				 "\nstdout:\n" + out_result.stdout_text + "\nstderr:\n" + out_result.stderr_text);
		return false;
	}
	return true;
}

void write_text(const fs::path &path, const std::string &contents)
{
	fs::create_directories(path.parent_path());
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
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

// TOML basic string
std::string quoted(const fs::path &path)
{
	return json(file_path_utf8(path)).dump();
}

} // namespace

int main(int argc, char **argv)
{
	const std::string cmake = option(argc, argv, "--cmake", "cmake");
	const std::string build_dir = option(argc, argv, "--build-dir", "");
	const std::string configuration = option(argc, argv, "--config", "");
	const std::string bindir = option(argc, argv, "--bindir", "bin");
	const std::string runtime_dir = option(argc, argv, "--runtime-dir", "share/odin");
	const std::string helper = option(argc, argv, "--helper", "");

	std::mt19937 engine{std::random_device{}()};
	const fs::path root =
	  fs::temp_directory_path() /
	  ("odin install test " + std::to_string(std::uniform_int_distribution<int>(0, 0xffffff)(engine)));

	// spaces in every path: this is where quoting defects surface
	const fs::path prefix = root / "install prefix";
	const fs::path project = root / "consuming project";
	const fs::path invocation = root / "unrelated invocation";
	fs::create_directories(project);
	fs::create_directories(invocation);

	std::vector<std::string> install = {cmake, "--install", build_dir, "--prefix",
										file_path_utf8(prefix)};
	if (!configuration.empty())
	{
		install.push_back("--config");
		install.push_back(configuration);
	}

	subprocess_result result;
	if (run(install, invocation, result))
	{
		const fs::path executable =
		  prefix / bindir /
#ifdef _WIN32
		  "odin.exe";
#else
		  "odin";
#endif
		const fs::path runtime = prefix / runtime_dir;

		for (const fs::path &required : {executable,
										 runtime / "harness" / "schemas" / "task.schema.json",
										 runtime / "harness" / "workflows" / "feature.json",
										 runtime / "harness" / "agents" / "implementer.json"})
		{
			require(fs::exists(required),
					"installed runtime is incomplete: " + file_path_utf8(required));
		}

		// the install tree must carry no interpreter and no scripts
		if (fs::exists(prefix))
		{
			for (const fs::directory_entry &entry : fs::recursive_directory_iterator(prefix))
			{
				const std::string name = entry.path().filename().string();
				const std::string extension = entry.path().extension().string();
				require(extension != ".py" && extension != ".pyc" && name != "requirements.txt",
						"install prefix contains Python: " + file_path_utf8(entry.path()));
			}
		}

		write_text(project / "project.txt", "installed layout sentinel\n");
		write_text(project / "task.json",
				   R"({"id":"installed-smoke","kind":"feature",)"
				   R"("title":"Installed layout smoke test",)"
				   R"("request":"Complete the deterministic installed-layout workflow.",)"
				   R"("context":{},"constraints":[]})");

		// the gate is a project-supplied command; a consuming project may use
		// any language, but Odin's own smoke test must not need one
		write_text(project / "odin.toml",
				   "[harness]\n"
				   "state_dir = \".odin/runs\"\n"
				   "max_total_transitions = 30\n\n"
				   "[git]\nstage_on_success = false\n\n"
				   "[adapters.smoke]\ntype = \"mock\"\n\n"
				   "[models.smoke]\nadapter = \"smoke\"\nmodel = \"installed-layout\"\n"
				   "tags = [\"test\"]\n\n"
				   "[routing]\ndefault = \"smoke\"\n\n"
				   "[gates.quality]\ncommand = [" +
					 json(helper).dump() +
					 ", \"out:installed project gate passed\"]\ntimeout_seconds = 30\n");

		const std::string odin = file_path_utf8(executable);
		const std::string config = file_path_utf8(project / "odin.toml");

		// run from a directory that is neither the install prefix nor the
		// project: runtime assets must resolve from the executable
		if (run({odin, "validate", "--self-only"}, invocation, result))
		{
			const json validated = json::parse(result.stdout_text, nullptr, false);
			require(!validated.is_discarded() && validated.value("status", "") == "valid",
					"installed definitions did not validate");
		}

		run({odin, "--config", config, "tools", "list"}, invocation, result);

		if (run({odin, "--config", config, "start", file_path_utf8(project / "task.json")},
				invocation, result))
		{
			const json payload = json::parse(result.stdout_text, nullptr, false);
			require(!payload.is_discarded(), "start did not emit JSON");
			if (!payload.is_discarded())
			{
				require(payload.at("state").at("status") == "complete",
						"installed run did not complete");

				const fs::path run_dir = fs::path(payload.at("run_dir").get<std::string>());
				odin_error err;
				const json state = json_read(run_dir / "state.json", err);
				const json context = json_read(run_dir / "context.json", err);
				require(!failed(err), "run state could not be read: " + err.message);

				require(state.value("schema_version", 0) == 2, "state is not schema version 2");
				require(state.value("status", "") == "complete", "state is not complete");
				require(context.at("history").size() == 8,
						"expected 8 history entries, found " +
						  std::to_string(context.at("history").size()));

				std::vector<std::string> started;
				std::vector<std::string> completed;
				std::size_t records = 0;
				for (const fs::directory_entry &entry :
					 fs::directory_iterator(run_dir / "journal"))
				{
					++records;
					const json record = json_read(entry.path(), err);
					const std::string type = record.value("type", "");
					const std::string id = record.value("execution_id", "");
					if (type == "stage_started")
						started.push_back(id);
					else if (type == "stage_completed")
						completed.push_back(id);
				}
				require(records == 16, "expected 16 journal records, found " +
										 std::to_string(records));
				std::sort(started.begin(), started.end());
				std::sort(completed.begin(), completed.end());
				require(started == completed, "a stage attempt was started but never completed");

				bool gate_seen = false;
				for (const json &record : context.at("history"))
				{
					if (record.value("kind", "") != "gate")
						continue;
					const std::string output =
					  record.at("result").at("artifacts").at("gate").value("output", "");
					gate_seen = output.find("installed project gate passed") != std::string::npos;
					break;
				}
				require(gate_seen, "the project-relative gate did not execute");
			}
		}

		require(!fs::exists(prefix / ".odin"),
				"mutable project state was written into the install prefix");
	}

	std::error_code ignored;
	fs::remove_all(root, ignored);

	if (failures == 0)
		std::printf("installed layout: ok\n");
	return failures == 0 ? 0 : 1;
}
