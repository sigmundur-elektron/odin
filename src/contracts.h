#pragma once
#include <filesystem>
#include <map>
#include <string>

#include "types.h"

// Odin's five owned schemas, loaded from disk and validated natively.
//
// This used to be a thin wrapper over a Python sidecar. It now owns the schema
// directory and the parsed-schema cache directly, which is what removes an
// interpreter from every run: validation happens on every stage, and the
// sidecar was started lazily but then lived for the whole process.
//
// A schema is read, checked for unsupported keywords, and cached on first use.
// Reading them is cheap; re-reading them 240 times in a run is not.
struct contracts
{
	std::filesystem::path schema_root; // the harness/schemas directory
	std::map<std::string, json> cache; // "task" -> parsed schema
};

void contracts_configure(contracts &c, const std::filesystem::path &schema_root);

// Validate `value` against one of the bundled schemas: "task", "handoff",
// "agent", "skill" or "workflow".
//
// `where` is the human-readable subject of the message - a file path for
// definitions, or a phrase like "stage 'review' output" for an agent's handoff.
// On failure out_error reads:
//
//   stage 'review' output violates handoff: status: "ok" is not one of [...]
void contract_validate(contracts &c,
					   const json &value,
					   const std::string &contract,
					   const std::string &where,
					   odin_error &out_error);
