#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "types.h"

// Run a child process to completion with a hard timeout.
//
// Named subprocess rather than process so it cannot shadow the CRT''s
// <process.h>: src/ is on the include path, and a header that hides a platform
// header is a trap.

// what to run and how
struct subprocess_options
{
	std::vector<std::string> command;
	std::filesystem::path working_directory;

	// Literal values overlaid on Odin's small operational baseline. The parent
	// environment is not inherited wholesale; additional names must be explicit.
	std::map<std::string, std::string> environment;
	std::vector<std::string> inherit_environment;
	bool redact_stdout = false;
	bool redact_stderr = false;

	// written to the child's stdin, which is then closed. stdin is always a
	// pipe, even when this is empty: a child that reads stdin then gets EOF
	// rather than consuming the harness's own input or blocking forever.
	std::string input;

	// fold stderr into stdout, as stderr=subprocess.STDOUT does for gates
	bool merge_stderr = false;

	// 0 waits forever, which is what git staging uses
	int timeout_seconds = 0;
};

struct subprocess_result
{
	int exit_code = 0;
	std::string stdout_text;
	std::string stderr_text; // always empty when merge_stderr is set
};

// on POSIX, writing to a pipe whose read end is gone raises SIGPIPE, whose
// default disposition kills the writer. every child odin starts has its stdin on
// a pipe, so any child that exits without draining what we send - an adapter that
// ignores stdin, a contract service that died on import - would take odin down
// with it. reproc documents this as the caller's responsibility: with SIGPIPE
// ignored, write() reports EPIPE and the existing "the child closed stdin, that
// is its prerogative" branches can actually run.
//
// safe to call repeatedly and from the spawn paths themselves; the disposition is
// installed once. it does not leak into children, because reproc resets every
// signal to SIG_DFL in the child between fork and exec. no-op on windows.
void subprocess_ignore_sigpipe();

bool subprocess_environment_build(const std::vector<std::string> &inherit,
								  const std::map<std::string, std::string> &configured,
								  std::map<std::string, std::string> &out_environment,
								  odin_error &out_error);

// Redact scoped secret values after JSON parsing as well as in raw diagnostics;
// serialized strings can escape quotes/newlines and bypass raw-byte matching.
void subprocess_redact_json(json &value, const subprocess_options &options);

// replace malformed utf-8 with u+fffd, one per maximal invalid subpart, which is
// what python's errors="replace" does. every subprocess call in the harness
// decodes that way, and the text lands in json state files, so a raw invalid
// byte would otherwise produce an artifact nothing can read back.
std::string utf8_sanitize(const std::string &bytes);

// render a command as a quoted list: ['git', 'add', '--', 'a b.txt'].
//
// this text reaches durable state through an adapter failure's handoff summary
// and is read by whoever debugs the run, so the format is a stable diagnostic
// surface rather than an internal detail. single quotes, switching to double
// when the value contains one.
std::string command_repr(const std::vector<std::string> &values);

// run a child to completion, writing stdin while draining stdout and stderr so
// neither side can deadlock on a full pipe.
//
// out_error is set only when the command could not be run at all: a failed
// launch or a timeout. a command that ran and failed is a SUCCESSFUL call with a
// nonzero exit_code. a gate that fails is data, not an error.
subprocess_result subprocess_run(const subprocess_options &options, odin_error &out_error);
