#pragma once
#include <filesystem>

#include "types.h"

// Paths owned by the application composition root. Runtime assets belong to
// Odin's installation; project files and child execution belong to the
// directory containing odin.toml.
struct runtime_paths
{
	std::filesystem::path runtime_root;
	std::filesystem::path project_root;
	std::filesystem::path config_path;
};

runtime_paths runtime_paths_resolve(const char *argv0,
									const std::filesystem::path &config_path);

// Confirm the resolved runtime root actually holds Odin's assets.
//
// Resolution falls back to the build-time source directory when the installed
// layout is not found, which is right for a source-tree build and wrong
// everywhere else. Checking it explicitly turns "somebody else's absolute path
// appeared in an error message" into one clear failure at startup.
bool runtime_paths_check(const runtime_paths &paths, odin_error &out_error);
