#pragma once
#include <filesystem>
#include <string>

#include "config.h"
#include "definitions.h"
#include "sidecar.h"
#include "types.h"

// the durable state machine.
//
// a run is a directory under [harness].state_dir holding task.json, context.json,
// state.json and one event file per executed stage. every transition is written
// through json_write_atomic, so a gui polling the directory always sees a whole
// file - that guarantee is the integration contract, not an optimisation.
struct engine
{
	project_config config;
	definitions *defs = nullptr;
	sidecar *service = nullptr;
};

struct engine_run_options
{
	std::string model_override;
	bool retry_interrupted = false;
};

// the three states a run can end in. anything else means it is still running.
bool engine_is_terminal(const std::string &status);

void engine_configure(engine &e,
					  const project_config &config,
					  definitions &defs,
					  sidecar &service);

// validate the task, create the run directory, and write its opening state.
// returns the run directory.
std::filesystem::path engine_create_run(engine &e,
										const json &task,
										const std::filesystem::path &task_file,
										odin_error &out_error);

// drive an existing run to a terminal state and return its final state object.
// `model_override` forces one configured profile for every agent stage; pass an
// empty string to use [routing].
//
// out_error is set only for defects the run cannot describe itself: an unknown
// stage, an unconfigured gate, a handoff that violates its contract. an agent
// that fails is recorded as a blocked handoff and the run continues to a
// terminal state normally.
json engine_run(engine &e,
				const std::filesystem::path &run_dir,
				const engine_run_options &options,
				odin_error &out_error);

// Compatibility overload for callers that only select a model.
json engine_run(engine &e,
				const std::filesystem::path &run_dir,
				const std::string &model_override,
				odin_error &out_error);
