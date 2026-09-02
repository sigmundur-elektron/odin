#include <doctest/doctest.h>

#include "executable.h"
#include "test_support.h"
#include "tools_command.h"

#include <cstdlib>
#include <string>

namespace fs = std::filesystem;

TEST_CASE("executable_find honours platform suffixes")
{
	const std::vector<std::string> &suffixes = executable_suffixes();
	REQUIRE_FALSE(suffixes.empty());

#ifdef _WIN32
	// .cmd before .exe: npm and every npm-installed CLI ship a .cmd shim, and
	// resolving a same-named .exe first would silently pick the wrong binary
	CHECK(suffixes.front() == ".cmd");
	CHECK(suffixes.back().empty());
#else
	CHECK(suffixes.size() == 1);
	CHECK(suffixes.front().empty());
#endif
}

TEST_CASE("executable_find_in searches the given directories in order")
{
	const temp_dir dir;
	const fs::path first = dir.path / "first";
	const fs::path second = dir.path / "second";

#ifdef _WIN32
	const std::string name = "odin-fixture-tool";
	temp_write(second / (name + ".cmd"), "@echo off\n");
	temp_write(first / (name + ".exe"), "not really an exe\n");
#else
	const std::string name = "odin-fixture-tool";
	temp_write(second / name, "#!/bin/sh\n");
	temp_write(first / name, "#!/bin/sh\n");
#endif

	// earlier directories win
	CHECK(executable_find_in(name, {first, second}).parent_path() == first);
	CHECK(executable_find_in(name, {second, first}).parent_path() == second);
	CHECK(executable_find_in(name, {}).empty());
	CHECK(executable_find_in("odin-no-such-tool-anywhere", {first, second}).empty());
}

TEST_CASE("executable_find_in accepts an explicit path without searching")
{
	const temp_dir dir;
	const fs::path exact = dir.path / "somewhere" / "explicit-tool";
	temp_write(exact, "x");

	// a path, not a bare name: the directory list is irrelevant
	CHECK(executable_find_in(exact.string(), {}) == exact);
	CHECK(executable_find_in((dir.path / "absent").string(), {}).empty());
}

TEST_CASE("executable_find resolves something known to exist on PATH")
{
#ifdef _WIN32
	CHECK_FALSE(executable_find("cmd").empty());
#else
	CHECK_FALSE(executable_find("sh").empty());
#endif
	CHECK(executable_find("odin-no-such-executable-anywhere").empty());
}

// ------------------------------------------------------------------ tools

TEST_CASE("the known tool set installs no Python runtime of its own")
{
	// the point of the port: wanting Aider must not make Python a dependency of
	// Odin. uv brings its own interpreter; pip would have needed ours.
	REQUIRE_FALSE(tools_known().empty());
	for (const tool_spec &spec : tools_known())
	{
		CAPTURE(spec.name);
		CHECK((spec.manager == "npm" || spec.manager == "uv"));
		CHECK(spec.manager != "pip");
		CHECK_FALSE(spec.package.empty());
		CHECK_FALSE(spec.binary.empty());
		CHECK_FALSE(spec.description.empty());
	}
}

TEST_CASE("tools are reported as installed only when the binary is really there")
{
	const temp_dir dir;
	const tool_spec &spec = tools_known().front();

	CHECK(tools_installed_path(dir.path, spec).empty());

	// creating the directory is not enough; a failed install leaves one behind
	std::filesystem::create_directories(dir.path / ".odin" / "tools" / spec.name);
	CHECK(tools_installed_path(dir.path, spec).empty());
}

TEST_CASE("every install layout a package manager might produce is probed")
{
	// which layout appears depends on the manager and the package, not on the
	// operating system, so all four are checked on every platform
	const char *layouts[] = {"node_modules/.bin", "Scripts", "bin"};

	for (const char *layout : layouts)
	{
		CAPTURE(layout);
		const temp_dir dir;
		const tool_spec &spec = tools_known().front();
		const fs::path base = dir.path / ".odin" / "tools" / spec.name / layout;

		for (const std::string &suffix : {std::string{}, std::string{".cmd"}, std::string{".exe"}})
		{
			const temp_dir inner;
			const fs::path where =
			  inner.path / ".odin" / "tools" / spec.name / layout / (spec.binary + suffix);
			temp_write(where, "x");
			const fs::path found = tools_installed_path(inner.path, spec);
			if (!found.empty())
			{
				CHECK(found.filename().string().rfind(spec.binary, 0) == 0);
			}
		}
	}
}

TEST_CASE("tools list succeeds with nothing installed")
{
	const temp_dir dir;
	tools_options options;
	options.subcommand = "list";
	CHECK(tools_command_run(dir.path, options) == 0);
}

TEST_CASE("an unknown tool is refused with the known set named")
{
	const temp_dir dir;
	tools_options options;
	options.subcommand = "install";
	options.name = "definitely-not-a-tool";
	CHECK(tools_command_run(dir.path, options) == 2);
}
