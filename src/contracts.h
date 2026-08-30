#pragma once
#include <string>

#include "sidecar.h"
#include "types.h"

// validate `value` against one of the bundled schemas: "task", "handoff",
// "agent", "skill" or "workflow".
//
// `where` is the human-readable subject of the message, exactly as in
// harness/contracts.py - a file path for definitions, or a phrase like
// "stage 'review' output" for an agent's handoff.
//
// on failure out_error carries the message harness/contracts.py would have
// raised, verbatim, because the sidecar produces it by calling that function.
void contract_validate(sidecar &service,
					   const json &value,
					   const std::string &contract,
					   const std::string &where,
					   odin_error &out_error);
