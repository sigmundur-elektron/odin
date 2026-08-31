#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

// hand a command straight to the unmodified python cli.
//
// doctor, tools and auth stay in python. they are rare, interactive, and full of
// platform-specific probing: %LOCALAPPDATA% and scoop shim tables, .cmd/.exe
// suffix resolution, https provider probes, npm and pip installs. none of it
// runs in the stage loop, and porting it would drag a tls client and a no-echo
// terminal prompt into C++ for no benefit.
//
// harness/cli.py dispatches all three BEFORE it loads any configuration, so they
// have no dependency on ported code and the seam is clean.
//
// stdin, stdout and stderr are inherited rather than captured, which is what
// keeps getpass from echoing and lets npm render progress live.
//
// Runtime and project locations are intentionally separate: odin.py belongs to
// the installed runtime, while relative config and project paths belong to the
// directory containing odin.toml.
int delegate_to_python(const std::string &interpreter,
					   const std::filesystem::path &runtime_root,
					   const std::filesystem::path &project_root,
					   const std::vector<std::string> &argv,
					   odin_error &out_error);

// true when `command` is one of the three python keeps
bool delegate_owns(const std::string &command);
