#pragma once
#include "config.h"
#include "subprocess.h"
#include "types.h"

// what one adapter invocation produced.
//
// an agent adapter is any executable: it reads a single json request on stdin,
// writes a single handoff on stdout, and exits nonzero to signal failure. that
// process boundary is odin's entire extension mechanism, which is why the two
// bundled adapters stay in python and are called rather than ported.
struct adapter_result
{
	json response; // the handoff object the adapter printed
	json metadata; // command, exit_code, stderr, model, timing - for the event log
};

// run the adapter named by `profile`, sending `request` on its stdin.
//
// every failure mode from harness/adapters.py is preserved: a launch failure or
// timeout, a nonzero exit, output that is not json, and output that is json but
// not an object.
adapter_result adapter_run(const command_spec &spec,
						   const model_profile &profile,
						   const json &request,
						   const project_config &config,
						   odin_error &out_error);
