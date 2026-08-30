#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "types.h"

// what to run and how. mirrors the arguments harness/adapters.py and
// harness/engine.py pass to subprocess.run.
struct process_options
{
	std::vector<std::string> command;
	std::filesystem::path working_directory;

	// overlaid on the inherited environment, matching python's
	// dict(os.environ) followed by update(config.environment).
	std::map<std::string, std::string> environment;

	// written to the child's stdin, which is then closed. stdin is always a
	// pipe, even when this is empty: a child that reads stdin then gets EOF
	// rather than consuming the harness's own input or blocking forever.
	std::string input;

	// fold stderr into stdout, as stderr=subprocess.STDOUT does for gates
	bool merge_stderr = false;

	// 0 waits forever, which is what `git add` does in harness/engine.py
	int timeout_seconds = 0;
};

struct process_result
{
	int exit_code = 0;
	std::string stdout_text;
	std::string stderr_text; // always empty when merge_stderr is set
};

// replace malformed utf-8 with u+fffd, one per maximal invalid subpart, which is
// what python's errors="replace" does. every subprocess call in the harness
// decodes that way, and the text lands in json state files, so a raw invalid
// byte would otherwise produce an artifact nothing can read back.
std::string utf8_sanitize(const std::string &bytes);

// render a command the way python renders a list of strings, so that timeout
// messages match: ['python', 'scripts/mock_agent.py']. this text reaches durable
// state via an adapter failure's handoff summary, so it is worth matching.
std::string python_list_repr(const std::vector<std::string> &values);

// run a child to completion, writing stdin while draining stdout and stderr so
// neither side can deadlock on a full pipe.
//
// out_error is set only when the command could not be run at all: a failed
// launch or a timeout. a command that ran and failed is a SUCCESSFUL call with a
// nonzero exit_code - the same distinction harness/adapters.py makes by testing
// returncode rather than passing check=True.
process_result process_run(const process_options &options, odin_error &out_error);
