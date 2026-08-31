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
// `root` is the directory holding odin.py, derived from --config the same way
// the sidecar locates scripts/contract_service.py.
int delegate_to_python(const std::string &interpreter,
					   const std::filesystem::path &root,
					   const std::vector<std::string> &argv,
					   odin_error &out_error);

// true when `command` is one of the three python keeps
bool delegate_owns(const std::string &command);
