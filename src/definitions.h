#pragma once
#include <filesystem>
#include <map>
#include <string>

#include "sidecar.h"
#include "types.h"

// agents, skills, workflows and templates are json files on disk, looked up by
// name. keeping them on disk rather than embedding them is what lets a workflow
// be edited without recompiling.
//
// they cannot change during a run, so each is read and validated exactly once.
// harness/definitions.py re-reads and re-validates on every stage, which is the
// bulk of the sidecar traffic a literal port would generate: caching takes a
// full run from roughly 240 validations down to one per stage.
struct definitions
{
	sidecar *service = nullptr;
	std::filesystem::path package_root; // the harness/ directory
	std::map<std::string, json> cache;	// "agents/analyst" -> value
};

void definitions_configure(definitions &d,
						   sidecar &service,
						   const std::filesystem::path &package_root);

// each returns a pointer into the cache, stable for the lifetime of `d`, or
// nullptr with out_error set.
const json *definitions_load_agent(definitions &d,
								   const std::string &identifier, odin_error &out_error);
const json *definitions_load_skill(definitions &d,
								   const std::string &identifier, odin_error &out_error);
const json *definitions_load_workflow(definitions &d,
									  const std::string &identifier, odin_error &out_error);
const json *definitions_load_template(definitions &d,
									  const std::string &identifier, odin_error &out_error);
