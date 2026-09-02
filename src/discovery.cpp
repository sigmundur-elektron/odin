#include "discovery.h"

#include <algorithm>
#include <cstdlib>

#include "atomic_file.h"
#include "executable.h"
#include "http_client.h"
#include "subprocess.h"

namespace fs = std::filesystem;

namespace
{

struct endpoint_spec
{
	const char *name;
	const char *base_url;
};

// One probe covers most of the ecosystem: Ollama, LM Studio, llama.cpp, vLLM,
// LiteLLM, OpenRouter, OpenAI and Azure all expose GET /v1/models.
const endpoint_spec default_endpoints[] = {
  {"ollama", "http://127.0.0.1:11434/v1"},
  {"lm-studio", "http://127.0.0.1:1234/v1"},
  {"llama-cpp", "http://127.0.0.1:8080/v1"},
  {"vllm", "http://127.0.0.1:8000/v1"},
  {"litellm", "http://127.0.0.1:4000/v1"},
};

struct hosted_spec
{
	const char *name;
	const char *key_env;
	const char *base_url;
};

// Hosted providers are linkable when their credential is present. Odin records
// the variable name, never the value.
const hosted_spec hosted_keys[] = {
  {"openai", "OPENAI_API_KEY", "https://api.openai.com/v1"},
  {"anthropic", "ANTHROPIC_API_KEY", "https://api.anthropic.com/v1"},
  {"openrouter", "OPENROUTER_API_KEY", "https://openrouter.ai/api/v1"},
  {"groq", "GROQ_API_KEY", "https://api.groq.com/openai/v1"},
  {"together", "TOGETHER_API_KEY", "https://api.together.xyz/v1"},
  {"mistral", "MISTRAL_API_KEY", "https://api.mistral.ai/v1"},
  {"deepseek", "DEEPSEEK_API_KEY", "https://api.deepseek.com/v1"},
};

struct agent_cli_spec
{
	const char *name;
	std::vector<std::string> enumerate_args;
};

const std::vector<agent_cli_spec> &known_agent_clis()
{
	static const std::vector<agent_cli_spec> known = {
	  {"opencode", {"models"}}, {"claude", {"--version"}}, {"aider", {"--version"}},
	  {"codex", {"--version"}}, {"llm", {"models"}},
	};
	return known;
}

std::string environment_value(const char *name)
{
#ifdef _WIN32
	char *value = nullptr;
	std::size_t size = 0;
	if (_dupenv_s(&value, &size, name) != 0 || value == nullptr)
		return {};
	std::string result(value);
	std::free(value);
	return result;
#else
	const char *value = std::getenv(name);
	return value == nullptr ? std::string{} : std::string(value);
#endif
}

std::string trimmed(const std::string &text)
{
	const std::size_t first = text.find_first_not_of(" \t\n\r\f\v");
	if (first == std::string::npos)
		return {};
	const std::size_t last = text.find_last_not_of(" \t\n\r\f\v");
	return text.substr(first, last - first + 1);
}

std::string without_trailing_slash(const std::string &url)
{
	std::string result = url;
	while (!result.empty() && result.back() == '/')
		result.pop_back();
	return result;
}

// Fetch and parse JSON. `out_status` carries the HTTP status so the caller can
// tell 401/403 (a key problem) from everything else (a reachability problem).
bool fetch_json(const std::string &url, const std::string &token, json &out_payload,
				int &out_status, std::string &out_detail)
{
	http_request request;
	request.url = url;
	request.timeout_seconds = discovery_probe_timeout_seconds;
	request.headers.push_back({"Accept", "application/json"});
	if (!token.empty())
		request.headers.push_back({"Authorization", "Bearer " + token});

	http_response response;
	odin_error err;
	if (!http_send(request, response, err))
	{
		out_detail = err.message;
		return false;
	}

	out_status = response.status;
	if (response.status < 200 || response.status >= 300)
	{
		out_detail = "HTTP " + std::to_string(response.status);
		return false;
	}

	out_payload = json::parse(response.body, nullptr, false);
	if (out_payload.is_discarded())
	{
		out_detail = "response was not JSON";
		return false;
	}
	return true;
}

// "http://127.0.0.1:11434/v1" -> host 127.0.0.1, port 11434
void split_host_port(const std::string &base_url, std::string &out_host, int &out_port)
{
	out_host.clear();
	out_port = 0;

	const std::size_t after_scheme = base_url.find("//");
	std::string rest =
	  after_scheme == std::string::npos ? base_url : base_url.substr(after_scheme + 2);
	const std::size_t slash = rest.find('/');
	if (slash != std::string::npos)
		rest = rest.substr(0, slash);

	const std::size_t colon = rest.find(':');
	if (colon == std::string::npos)
	{
		out_host = rest;
		return;
	}
	out_host = rest.substr(0, colon);
	const std::string port_text = rest.substr(colon + 1);
	if (!port_text.empty() &&
		port_text.find_first_not_of("0123456789") == std::string::npos)
		out_port = std::stoi(port_text);
}

discovered_provider probe_openai_endpoint(const std::string &name, const std::string &base_url,
										  const std::string &api_key_env)
{
	discovered_provider provider;
	provider.name = name;
	provider.transport = "openai-compatible";
	provider.status = "unreachable";
	provider.base_url = base_url;
	provider.api_key_env = api_key_env;

	std::string host;
	int port = 0;
	split_host_port(base_url, host, port);

	// only local endpoints get the pre-check: a remote host that is merely slow
	// should not be written off by a 350ms budget
	if ((host == "127.0.0.1" || host == "localhost") && port > 0)
	{
		if (!http_port_open(host, port, discovery_port_timeout_ms))
		{
			provider.detail = "no listener on port";
			return provider;
		}
	}

	const std::string token = api_key_env.empty() ? std::string{} : environment_value(api_key_env.c_str());

	json payload;
	int status = 0;
	std::string detail;
	if (!fetch_json(without_trailing_slash(base_url) + "/models", token, payload, status, detail))
	{
		if (status == 401 || status == 403)
			provider.status = "auth-required";
		else if (status != 0)
			provider.status = "error";
		provider.detail = detail;
		return provider;
	}

	provider.status = "ready";
	for (const std::string &identifier : discovery_parse_openai_models(payload))
	{
		discovered_model model;
		model.id = identifier;
		model.provider = name;
		model.transport = "openai-compatible";
		model.base_url = base_url;
		model.api_key_env = api_key_env;
		provider.models.push_back(std::move(model));
	}
	provider.detail = std::to_string(provider.models.size()) + " model(s)";
	return provider;
}

bool probe_ollama_native(discovered_provider &out_provider)
{
	if (!http_port_open("127.0.0.1", 11434, discovery_port_timeout_ms))
		return false;

	out_provider = discovered_provider{};
	out_provider.name = "ollama";
	out_provider.transport = "openai-compatible";
	out_provider.status = "unreachable";
	out_provider.base_url = "http://127.0.0.1:11434/v1";

	json payload;
	int status = 0;
	std::string detail;
	// the native endpoint reports parameter size and quantization, which the
	// OpenAI-compatible one does not
	if (!fetch_json("http://127.0.0.1:11434/api/tags", "", payload, status, detail))
	{
		out_provider.detail = detail;
		return true;
	}

	out_provider.status = "ready";
	out_provider.models = discovery_parse_ollama_tags(payload);
	out_provider.detail = std::to_string(out_provider.models.size()) + " model(s), native tags";
	return true;
}

bool probe_agent_cli(const agent_cli_spec &spec, const discovery_options &options,
					 discovered_provider &out_provider)
{
	std::vector<fs::path> search;
	for (const fs::path &extra : options.extra_paths)
		search.push_back(extra);
	for (const fs::path &candidate : discovery_candidate_dirs(options.project_root))
		search.push_back(candidate);

	fs::path executable = executable_find(spec.name);
	std::string source = "PATH";
	if (executable.empty())
	{
		executable = executable_find_in(spec.name, search);
		if (executable.empty())
			return false;
		source = file_path_utf8(executable.parent_path());
	}

	out_provider = discovered_provider{};
	out_provider.name = spec.name;
	out_provider.transport = "cli-agent";
	out_provider.status = "ready";
	out_provider.command = file_path_utf8(executable);
	out_provider.detail =
	  source == "PATH" ? "on PATH" : "found in " + source + ", not on PATH";

	if (!options.deep)
	{
		out_provider.detail += "; run with --deep to enumerate models";
		return true;
	}

	subprocess_options run;
	run.command.push_back(file_path_utf8(executable));
	for (const std::string &argument : spec.enumerate_args)
		run.command.push_back(argument);
	run.timeout_seconds = 20;
	run.working_directory = options.project_root;

	odin_error err;
	const subprocess_result completed = subprocess_run(run, err);
	if (failed(err))
	{
		out_provider.detail = "enumeration failed: " + err.message;
		return true;
	}
	if (completed.exit_code != 0)
	{
		// a CLI that is installed but not logged in exits nonzero; that is a
		// credential problem, not an absence
		out_provider.status = "auth-required";
		std::string detail = trimmed(completed.stderr_text.empty() ? completed.stdout_text
																  : completed.stderr_text);
		if (detail.size() > 200)
			detail.resize(200);
		out_provider.detail = detail;
		return true;
	}

	// a model id is a non-empty line containing '/' and no space. crude, but it
	// separates identifiers from the prose these tools print around them.
	std::size_t start = 0;
	while (start <= completed.stdout_text.size())
	{
		const std::size_t end = completed.stdout_text.find('\n', start);
		const std::string line = trimmed(completed.stdout_text.substr(
		  start, end == std::string::npos ? std::string::npos : end - start));
		if (!line.empty() && line.find('/') != std::string::npos &&
			line.find(' ') == std::string::npos)
		{
			discovered_model model;
			model.id = line;
			model.provider = spec.name;
			model.transport = "cli-agent";
			out_provider.models.push_back(std::move(model));
		}
		if (end == std::string::npos)
			break;
		start = end + 1;
	}

	out_provider.detail = out_provider.models.empty()
							? "reachable"
							: std::to_string(out_provider.models.size()) + " model(s)";
	return true;
}

// TOML basic-string quoting
std::string quoted(const std::string &value)
{
	return json(value).dump(-1, ' ', true, json::error_handler_t::replace);
}

} // namespace

std::vector<std::string> discovery_parse_openai_models(const json &payload)
{
	std::vector<std::string> identifiers;
	if (!payload.is_object())
		return identifiers;
	const auto data = payload.find("data");
	if (data == payload.end() || !data->is_array())
		return identifiers;
	for (const json &entry : *data)
	{
		if (!entry.is_object())
			continue;
		const auto id = entry.find("id");
		if (id != entry.end() && id->is_string())
			identifiers.push_back(id->get<std::string>());
	}
	std::sort(identifiers.begin(), identifiers.end());
	return identifiers;
}

json discovery_parse_parameter_billions(const json &text)
{
	if (!text.is_string())
		return nullptr;

	std::string cleaned = trimmed(text.get<std::string>());
	if (!cleaned.empty() && (cleaned.back() == 'B' || cleaned.back() == 'b'))
		cleaned.pop_back();
	if (cleaned.empty())
		return nullptr;

	// std::stod would accept "inf", "nan" and leading whitespace-plus-sign
	// forms that are not sizes; require a plain decimal number instead
	if (cleaned.find_first_not_of("0123456789.") != std::string::npos)
		return nullptr;
	if (std::count(cleaned.begin(), cleaned.end(), '.') > 1)
		return nullptr;

	try
	{
		return json(std::stod(cleaned));
	}
	catch (const std::exception &)
	{
		return nullptr;
	}
}

std::vector<discovered_model> discovery_parse_ollama_tags(const json &payload)
{
	std::vector<discovered_model> discovered;
	if (!payload.is_object())
		return discovered;
	const auto models = payload.find("models");
	if (models == payload.end() || !models->is_array())
		return discovered;

	for (const json &entry : *models)
	{
		if (!entry.is_object())
			continue;
		std::string name;
		for (const char *key : {"name", "model"})
		{
			const auto found = entry.find(key);
			if (found != entry.end() && found->is_string())
			{
				name = found->get<std::string>();
				break;
			}
		}
		if (name.empty())
			continue;

		const auto details = entry.find("details");
		const json empty = json::object();
		const json &fields =
		  details != entry.end() && details->is_object() ? *details : empty;

		discovered_model model;
		model.id = name;
		model.provider = "ollama";
		model.transport = "openai-compatible";
		model.base_url = "http://127.0.0.1:11434/v1";
		model.parameter_billions =
		  discovery_parse_parameter_billions(fields.value("parameter_size", json(nullptr)));
		model.detail = json{{"quantization", fields.value("quantization_level", json(nullptr))},
							{"family", fields.value("family", json(nullptr))}};
		discovered.push_back(std::move(model));
	}

	std::sort(discovered.begin(), discovered.end(),
			  [](const discovered_model &left, const discovered_model &right) {
				  return left.id < right.id;
			  });
	return discovered;
}

std::vector<fs::path> discovery_candidate_dirs(const fs::path &project_root)
{
	std::vector<fs::path> candidates;
	std::error_code code;

	candidates.push_back(project_root / "node_modules" / ".bin");

	const fs::path tools = project_root / ".odin" / "tools";
	if (fs::is_directory(tools, code))
	{
		candidates.push_back(tools);
		for (const fs::directory_entry &entry : fs::directory_iterator(tools, code))
		{
			const fs::path nested = entry.path() / "node_modules" / ".bin";
			if (fs::is_directory(nested, code))
				candidates.push_back(nested);
		}
	}

#ifdef _WIN32
	const std::string local = environment_value("LOCALAPPDATA");
	const std::string roaming = environment_value("APPDATA");
	const std::string profile = environment_value("USERPROFILE");
	if (!roaming.empty())
		candidates.push_back(fs::path(roaming) / "npm");
	if (!local.empty())
	{
		candidates.push_back(fs::path(local) / "Microsoft" / "WinGet" / "Links");
		candidates.push_back(fs::path(local) / "Programs");
	}
	if (!profile.empty())
		candidates.push_back(fs::path(profile) / "scoop" / "shims");
#else
	const std::string home = environment_value("HOME");
	if (!home.empty())
		candidates.push_back(fs::path(home) / ".local" / "bin");
	candidates.push_back(fs::path("/usr/local/bin"));
	candidates.push_back(fs::path("/opt/homebrew/bin"));
#endif

	std::vector<fs::path> present;
	for (const fs::path &candidate : candidates)
	{
		if (fs::is_directory(candidate, code))
			present.push_back(candidate);
	}
	return present;
}

std::vector<discovered_provider> discovery_run(const discovery_options &options)
{
	std::vector<discovered_provider> providers;

	discovered_provider ollama;
	const bool has_native_ollama = probe_ollama_native(ollama);
	if (has_native_ollama)
		providers.push_back(ollama);

	for (const endpoint_spec &endpoint : default_endpoints)
	{
		// the native probe already covered Ollama and reports more
		if (has_native_ollama && std::string(endpoint.name) == "ollama")
			continue;
		discovered_provider provider = probe_openai_endpoint(endpoint.name, endpoint.base_url, "");
		// a local port with nothing on it is not worth reporting as a failure
		if (provider.status != "unreachable" || provider.detail != "no listener on port")
			providers.push_back(std::move(provider));
	}

	const std::string custom = environment_value("OPENAI_BASE_URL");
	if (!custom.empty())
		providers.push_back(probe_openai_endpoint("custom", custom, "OPENAI_API_KEY"));

	if (options.include_hosted)
	{
		for (const hosted_spec &hosted : hosted_keys)
		{
			if (!environment_value(hosted.key_env).empty())
				providers.push_back(
				  probe_openai_endpoint(hosted.name, hosted.base_url, hosted.key_env));
		}
	}

	for (const agent_cli_spec &spec : known_agent_clis())
	{
		discovered_provider provider;
		if (probe_agent_cli(spec, options, provider))
			providers.push_back(std::move(provider));
	}

	return providers;
}

json discovery_to_json(const std::vector<discovered_provider> &providers)
{
	json all = json::array();
	for (const discovered_provider &provider : providers)
	{
		json models = json::array();
		for (const discovered_model &model : provider.models)
		{
			models.push_back(json{
			  {"id", model.id},
			  {"provider", model.provider},
			  {"transport", model.transport},
			  {"base_url", model.base_url.empty() ? json(nullptr) : json(model.base_url)},
			  {"api_key_env",
			   model.api_key_env.empty() ? json(nullptr) : json(model.api_key_env)},
			  {"parameter_billions", model.parameter_billions},
			  {"context_tokens", model.context_tokens},
			  {"detail", model.detail}});
		}
		all.push_back(json{
		  {"name", provider.name},
		  {"transport", provider.transport},
		  {"status", provider.status},
		  {"detail", provider.detail},
		  {"base_url", provider.base_url.empty() ? json(nullptr) : json(provider.base_url)},
		  {"api_key_env",
		   provider.api_key_env.empty() ? json(nullptr) : json(provider.api_key_env)},
		  {"command", provider.command.empty() ? json(nullptr) : json(provider.command)},
		  {"models", models}});
	}
	return all;
}

std::string discovery_emit_config(const std::vector<discovered_provider> &providers, int limit)
{
	std::vector<std::string> adapters;
	std::vector<std::string> profiles;
	std::vector<std::string> seen;

	for (const discovered_provider &provider : providers)
	{
		if (provider.status != "ready" || provider.models.empty())
			continue;

		const bool http = provider.transport == "openai-compatible";
		const std::string adapter = (http ? "http-" : "cli-") + provider.name;

		if (std::find(seen.begin(), seen.end(), adapter) == seen.end())
		{
			seen.push_back(adapter);
			std::string block = "[adapters." + adapter + "]\n";
			if (http)
			{
				// a built-in type, not an interpreter and a script path. an
				// emitted config that named python would defeat the whole port.
				block += "type = \"openai-compatible\"\n";
				block += "base_url = " + quoted(provider.base_url) + "\n";
				if (!provider.api_key_env.empty())
				{
					block += "api_key_env = " + quoted(provider.api_key_env) + "\n";
					block += "inherit_environment = [" + quoted(provider.api_key_env) + "]\n";
				}
				block += "timeout_seconds = 900\n";
			}
			else
			{
				block += "type = \"cli-agent\"\n";
				block += "command = [" +
						 quoted(provider.command.empty() ? provider.name : provider.command) +
						 ", \"run\", \"--model\", \"{model}\"]\n";
				block += "timeout_seconds = 1200\n";
			}
			adapters.push_back(block);
		}

		int emitted = 0;
		for (const discovered_model &model : provider.models)
		{
			if (emitted++ >= limit)
				break;
			// profile names are toml keys; the characters model ids use are not
			std::string profile = model.id;
			for (char &character : profile)
			{
				if (character == '/' || character == ':' || character == '.')
					character = '-';
			}
			std::string block = "[models." + profile + "]\n";
			block += "adapter = " + quoted(adapter) + "\n";
			block += "model = " + quoted(model.id) + "\n";
			if (model.parameter_billions.is_number())
				block += "parameter_billions = " + model.parameter_billions.dump() + "\n";
			block += "tags = [" + quoted(provider.name) + ", \"discovered\"]\n";
			profiles.push_back(block);
		}
	}

	if (adapters.empty() && profiles.empty())
		return "# No reachable providers were discovered.\n";

	std::string text;
	for (const std::string &block : adapters)
		text += (text.empty() ? "" : "\n") + block;
	for (const std::string &block : profiles)
		text += (text.empty() ? "" : "\n") + block;
	return text;
}
