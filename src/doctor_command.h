#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

// `odin doctor` - report what this machine can actually reach right now.
//
// Exit codes are a contract: 0 when at least one provider is ready, 1 when
// nothing is. --emit-config always exits 0, because "here is a config for
// nothing" is still a successful answer to what was asked.
struct doctor_options
{
	bool deep = false;
	bool as_json = false;
	bool emit_config = false;
	std::vector<std::filesystem::path> extra_paths;
};

int doctor_command_run(const std::filesystem::path &project_root, const doctor_options &options);
