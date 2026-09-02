#pragma once
#include <filesystem>
#include <string>

#include "types.h"

// `odin auth` - store provider credentials for agents and subagents.
//
// Exit codes are a contract the GUI and CI scripts depend on:
//   0  the operation succeeded
//   1  nothing to report, or nothing to remove (a query miss, not a failure)
//   2  the operation failed; the message is on stderr
struct auth_options
{
	std::string subcommand; // set | list | remove | import

	std::string name;
	std::string value; // `set --value`
	bool read_stdin = false;
	std::string note;
	bool as_json = false;

	std::string from_file; // `import --from-file`
	std::string provider;  // `import --provider`
};

int auth_command_run(const std::filesystem::path &project_root, const auth_options &options);
