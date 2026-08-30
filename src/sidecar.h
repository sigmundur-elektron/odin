#pragma once
#include <filesystem>
#include <memory>
#include <string>

#include <reproc++/reproc.hpp>

#include "types.h"

// a long-lived python child that answers contract validations over newline
// delimited json. the protocol lives in scripts/contract_service.py.
//
// jsonschema is odin's only third-party dependency and the one piece a C++
// rewrite cannot cheaply reproduce, so it is kept rather than reimplemented.
// validation runs once per stage, and a one-shot subprocess per call would cost
// roughly twenty seconds of process startup across a full run.
//
// the child is started lazily on the first call, so `odin status` never pays for
// it. one restart is allowed after an unexpected exit; a service that dies twice
// is reported rather than retried.
struct sidecar
{
	std::filesystem::path root; // cwd for the child, and where scripts/ lives
	std::string interpreter;

	std::unique_ptr<reproc::process> child;
	std::string buffered;		   // read from stdout, not yet a whole line
	std::string working_directory; // reproc keeps the pointer, so we keep the string

	int next_request_id = 1;
	int restarts = 0;
};

// how long one request may take before the child is declared hung. deliberately
// generous: schema validation is sub-millisecond, so this only fires on a defect
// and exists so a wedged sidecar cannot stall a whole run.
constexpr int sidecar_timeout_seconds = 5;

// ODIN_PYTHON wins, otherwise "python" - the same assumption odin.toml and
// harness/tools.py already make.
std::string sidecar_default_interpreter();

void sidecar_configure(sidecar &s,
					   const std::filesystem::path &root,
					   const std::string &interpreter);

// send one request and wait for its reply, starting or restarting the child as
// needed. `request` is given an "id" here; callers supply "op" and its operands.
json sidecar_call(sidecar &s, json request, odin_error &out_error);

// ask the child to exit, then insist. safe to call on a sidecar never started.
void sidecar_stop(sidecar &s);
