#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "types.h"

// Runtime discovery of reachable model providers.
//
// Odin bundles no provider and hardcodes no model id. It probes this machine for
// endpoints and agent CLIs that are reachable right now and reports what it
// finds, so configuration is written against observed reality instead of guessed
// names.
//
// Every probe is short-timeout and read-only, and a closed local port is
// rejected by a sub-second TCP pre-connect rather than a full HTTP timeout, so
// `doctor` stays fast enough for a GUI to call on demand.

constexpr int discovery_probe_timeout_seconds = 2;
constexpr int discovery_port_timeout_ms = 350;

struct discovered_model
{
	std::string id;
	std::string provider;
	std::string transport;
	std::string base_url;
	std::string api_key_env;
	json parameter_billions = nullptr;
	json context_tokens = nullptr;
	json detail = json::object();
};

struct discovered_provider
{
	std::string name;
	std::string transport;                 // openai-compatible | cli-agent
	std::string status = "unreachable";    // ready | auth-required | error | unreachable
	std::string detail;
	std::string base_url;
	std::string api_key_env;
	std::string command;
	std::vector<discovered_model> models;
};

struct discovery_options
{
	bool deep = false;
	bool include_hosted = true;
	std::vector<std::filesystem::path> extra_paths;
	std::filesystem::path project_root;
};

std::vector<discovered_provider> discovery_run(const discovery_options &options);

json discovery_to_json(const std::vector<discovered_provider> &providers);

// Paste-ready odin.toml blocks for what was actually observed, so model ids are
// never guessed. Emits built-in adapter types; it must never emit an interpreter
// or a script path.
std::string discovery_emit_config(const std::vector<discovered_provider> &providers, int limit);

// --- exposed for tests -----------------------------------------------------

std::vector<std::string> discovery_parse_openai_models(const json &payload);

// "32.8B" -> 32.8, "7B" -> 7. Null when the text is absent or unparseable.
json discovery_parse_parameter_billions(const json &text);

std::vector<discovered_model> discovery_parse_ollama_tags(const json &payload);

// Directories that commonly hold agent CLIs but are often absent from PATH:
// npm global prefixes, Scoop and WinGet shims, ~/.local/bin, and .odin/tools.
// A PATH-only probe reports "not installed" for tools that are installed.
std::vector<std::filesystem::path> discovery_candidate_dirs(
  const std::filesystem::path &project_root);
