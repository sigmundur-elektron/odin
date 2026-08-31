#pragma once
#include <filesystem>

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
