#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

// `odin tools` - optional agent-CLI installation, kept out of the default path.
//
// Odin installs nothing on its own. When a user asks explicitly, a tool is
// installed under .odin/tools/<name>/, which is gitignored and already searched
// by discovery, so nothing lands in the repository root or in `git status`.
//
// Python-implemented third-party tools are installed with `uv`, not `pip`. uv
// brings its own interpreter, so wanting Aider does not make Python a dependency
// of Odin itself - which was the whole point of the port.

struct tool_spec
{
	std::string name;
	std::string package;
	std::string manager; // npm | uv
	std::string binary;
	std::string description;
};

const std::vector<tool_spec> &tools_known();

// Absolute path to an installed tool's binary, or empty when it is not present.
std::filesystem::path tools_installed_path(const std::filesystem::path &project_root,
										   const tool_spec &spec);

struct tools_options
{
	std::string subcommand; // list | install
	std::string name;
	int timeout_seconds = 900;
};

int tools_command_run(const std::filesystem::path &project_root, const tools_options &options);
