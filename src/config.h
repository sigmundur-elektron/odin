#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "types.h"

// one entry from [adapters.*] or [gates.*]
//
// `type` selects a built-in adapter kind. The default, "command", runs an
// external executable and is what keeps Odin language-neutral: a consuming
// project can still point an adapter at a script in any language.
//
// A built-in kind runs in-process instead, and requires no `command`. This is
// the seam the native openai-compatible and cli-agent adapters plug into; "mock"
// is the first and exists so Odin's own checked-in configuration can execute a
// workflow without depending on an interpreter to run a fixture script.
struct command_spec
{
	std::string type = "command";
	std::vector<std::string> command;
	int timeout_seconds = default_timeout_seconds;
	std::vector<std::string> inherit_environment;
	std::map<std::string, std::string> environment;

	// --- built-in adapter settings ------------------------------------
	// Ignored by type = "command", which is configured entirely by argv.

	std::string base_url;    // openai-compatible
	std::string api_key_env; // environment variable holding the key
	std::string credential;  // name of a credential stored by `odin auth set`

	// cli-agent: the variable the resolved secret is injected into. The secret
	// never reaches argv, because process listings are readable by other
	// processes of the same user.
	std::string credential_env;
	bool prompt_stdin = false;      // pass the prompt on stdin, not as an argument
	std::string output;             // "" for one blob, "jsonl" for an event stream
	std::string text_path = "part.text";

	double temperature = 0.0;
	int max_context_chars = 24000;
	bool json_mode = true; // send response_format; some servers reject it
};

// a built-in kind is executed by Odin rather than spawned.
bool command_spec_is_builtin(const command_spec &spec);

// one entry from [models.*]
struct model_profile
{
	profile_id name;
	std::string adapter;
	std::string model;

	// "float | None" in python, and the toml distinguishes 0 from 0.0. held as a
	// json value so the integer / float / null distinction survives all the way
	// into the state files, where a stray "0" vs "0.0" breaks byte parity.
	json parameter_billions = nullptr;
	json context_tokens = nullptr;

	std::vector<std::string> tags;
};

// the whole of odin.toml. maps are ordered, matching the sorted() calls the
// python cli applies before printing.
struct project_config
{
	std::filesystem::path root;
	std::filesystem::path state_dir;

	std::map<std::string, command_spec> adapters;
	std::map<std::string, command_spec> gates;
	std::map<profile_id, model_profile> models;
	std::map<std::string, std::string> routing;
	std::map<std::string, std::string> environment;

	bool stage_on_success = false;
	int git_timeout_seconds = default_timeout_seconds;
	int max_total_transitions = default_max_total_transitions;
};

// parse odin.toml. `root` is derived from the config path's parent, and becomes
// the working directory for every child process odin launches.
project_config config_load(const std::filesystem::path &path, odin_error &out_error);

// resolve an agent role to a model profile: an explicit override first, then the
// [routing] entry for the role, then routing.default. borrowed, never owned;
// returns nullptr with out_error set when nothing matches.
const model_profile *config_model_for(const project_config &config,
									  const agent_id &agent,
									  const std::string &override_name,
									  odin_error &out_error);

const command_spec *config_adapter_for(const project_config &config,
									   const model_profile &profile,
									   odin_error &out_error);
