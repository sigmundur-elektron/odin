#include "adapter.h"

#include <chrono>
#include <cmath>

#include "atomic_file.h"
#include "credentials.h"
#include "http_client.h"
#include "output_extract.h"
#include "prompt_builder.h"

// harness/adapters.py uses str.format here, which also gives meaning to any
// stray brace in a configured command. plain substitution is used instead: it
// cannot raise on a command containing '{', and it is what
// adapters/cli_agent.py already does with str.replace.
static std::string adapter_expand(const std::string &part, const std::string &model)
{
	static const std::string token = "{model}";

	std::string out;
	std::size_t at = 0;
	for (;;)
	{
		const std::size_t found = part.find(token, at);
		if (found == std::string::npos)
		{
			out.append(part, at, std::string::npos);
			return out;
		}
		out.append(part, at, found - at);
		out += model;
		at = found + token.size();
	}
}

// The built-in deterministic agent.
//
// This is test infrastructure: it proves workflow mechanics and contract
// handling, and it is not a model. It used to be scripts/mock_agent.py, spawned
// as a child, which meant Odin's own checked-in odin.toml could not run a single
// workflow without a Python interpreter on PATH.
//
// Running it in-process rather than shipping a native fixture binary keeps it
// out of the install tree entirely while still letting the default config work.
static json adapter_mock(const json &request, const std::string &model)
{
	std::string agent;
	const auto agent_node = request.find("agent");
	if (agent_node != request.end() && agent_node->is_object())
	{
		const auto id = agent_node->find("id");
		if (id != agent_node->end() && id->is_string())
			agent = id->get<std::string>();
	}

	json artifacts = json::object();
	if (agent == "analyst")
	{
		json requested = json::array();
		const auto task = request.find("task");
		if (task != request.end() && task->is_object() && task->contains("request"))
			requested.push_back(task->at("request"));
		artifacts["requirements"] = requested;
		artifacts["acceptance_criteria"] = json::array({"Configured quality gate exits 0."});
		artifacts["non_goals"] = json::array();
		artifacts["changed_files"] = json::array({"README.md"});
	}
	else if (agent == "reproducer")
	{
		artifacts["reproduced"] = true;
		artifacts["command"] = json::array({"mock"});
		artifacts["exit_code"] = 0;
	}
	else if (agent == "implementer")
	{
		artifacts["changed_files"] = json::array({"README.md"});
		artifacts["notes"] = json::array({"mock implementation"});
	}
	else if (agent == "verifier")
	{
		artifacts["criteria"] = json::array({json{{"id", "A1"}, {"status", "passed"}}});
		artifacts["gaps"] = json::array();
	}
	else if (agent == "finalizer")
	{
		artifacts["summary"] = "mock workflow complete";
		artifacts["changed_files"] = json::array({"README.md"});
	}
	else
	{
		artifacts["findings"] = json::array();
	}

	json handoff;
	handoff["status"] = "approved";
	handoff["summary"] = agent + " approved using " + model;
	handoff["artifacts"] = artifacts;
	handoff["findings"] = json::array();
	return handoff;
}

// Resolve the secret this adapter should use, if any. Absent is not an error:
// a local server needs no key.
static bool adapter_secret(const command_spec &spec, const project_config &config,
						   std::string &out_secret, odin_error &out_error)
{
	credential_store store;
	credential_store_configure(store, config.root);
	bool found = false;
	if (!credential_resolve(store, spec.credential, spec.api_key_env, out_secret, found,
							out_error))
		return false;
	if (!found)
		out_secret.clear();
	return true;
}

// POST /chat/completions against any OpenAI-compatible endpoint.
//
// One retry is allowed, and only for the specific case worth retrying: an HTTP
// 400 while JSON mode is on. Several servers advertise the OpenAI shape but
// reject response_format, and the failure is indistinguishable from a bad
// request until it is tried without.
static json adapter_openai(const command_spec &spec, const model_profile &profile,
						   const json &request, const project_config &config,
						   json &out_metadata, odin_error &out_error)
{
	std::string secret;
	if (!adapter_secret(spec, config, secret, out_error))
		return json::object();

	const prompt_messages messages = prompt_build(request, spec.max_context_chars);

	json payload;
	payload["model"] = profile.model;
	payload["messages"] = json::array({json{{"role", "system"}, {"content", messages.system}},
									   json{{"role", "user"}, {"content", messages.user}}});
	payload["temperature"] = spec.temperature;

	std::string url = spec.base_url;
	while (!url.empty() && url.back() == '/')
		url.pop_back();
	url += "/chat/completions";

	http_response response;
	for (int attempt = 0; attempt < 2; ++attempt)
	{
		const bool json_mode = spec.json_mode && attempt == 0;
		if (json_mode)
			payload["response_format"] = json{{"type", "json_object"}};
		else
			payload.erase("response_format");

		http_request call;
		call.method = "POST";
		call.url = url;
		call.timeout_seconds = spec.timeout_seconds;
		call.body = payload.dump();
		call.headers.push_back({"Content-Type", "application/json"});
		if (!secret.empty())
			call.headers.push_back({"Authorization", "Bearer " + secret});

		odin_error transport;
		if (!http_send(call, response, transport))
		{
			fail(out_error, error_kind::adapter,
				 "adapter '" + profile.adapter + "' failed to run: " +
				   credential_redact(transport.message, secret));
			return json::object();
		}

		if (response.status == 400 && json_mode)
			continue; // retry once without JSON mode
		break;
	}

	out_metadata["status"] = response.status;

	if (response.status < 200 || response.status >= 300)
	{
		std::string detail = credential_redact(response.body, secret);
		if (detail.size() > 400)
			detail.resize(400);
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' exited " + std::to_string(response.status) + ": " +
			   detail);
		return json::object();
	}

	const json reply = json::parse(response.body, nullptr, false);
	std::string content;
	if (reply.is_object())
	{
		const auto choices = reply.find("choices");
		if (choices != reply.end() && choices->is_array() && !choices->empty())
		{
			const json &first = choices->at(0);
			if (first.is_object())
			{
				const auto message = first.find("message");
				if (message != first.end() && message->is_object())
				{
					const auto text = message->find("content");
					// content may be present and null when a model returns only
					// a refusal or tool call
					if (text != message->end() && text->is_string())
						content = text->get<std::string>();
				}
			}
		}
	}

	json handoff;
	odin_error extraction;
	if (!output_extract_object(content, handoff, extraction))
	{
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' returned invalid JSON: " +
			   credential_redact(extraction.message, secret));
		return json::object();
	}
	return handoff;
}

// Wrap an external coding-agent CLI.
//
// The secret is injected into the child's environment, never onto argv.
static json adapter_cli_agent(const command_spec &spec, const model_profile &profile,
							  const json &request, const project_config &config,
							  json &out_metadata, odin_error &out_error)
{
	std::string secret;
	if (!adapter_secret(spec, config, secret, out_error))
		return json::object();

	const std::string prompt = prompt_build_single(request, spec.max_context_chars);

	subprocess_options options;
	for (const std::string &part : spec.command)
		options.command.push_back(adapter_expand(part, profile.model));
	options.working_directory = config.root;
	options.environment = config.environment;
	for (const auto &[name, value] : spec.environment) options.environment[name] = value;
	options.inherit_environment = spec.inherit_environment;
	options.environment["ODIN_PROJECT_ROOT"] = file_path_utf8(config.root);
	if (!secret.empty() && !spec.credential_env.empty())
		options.environment[spec.credential_env] = secret;
	options.redact_stderr = true;
	options.timeout_seconds = spec.timeout_seconds;

	// a large artifact set can exceed the command-line length limit, so stdin
	// is the escape hatch rather than the default
	if (spec.prompt_stdin)
		options.input = prompt;
	else
		options.command.push_back(prompt);

	odin_error run_error;
	const subprocess_result completed = subprocess_run(options, run_error);
	if (failed(run_error))
	{
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' failed to run: " +
			   credential_redact(run_error.message, secret));
		return json::object();
	}

	out_metadata["command"] = options.command;
	out_metadata["exit_code"] = completed.exit_code;
	out_metadata["stderr"] = credential_redact(completed.stderr_text, secret);

	if (completed.exit_code != 0)
	{
		std::string detail =
		  credential_redact(completed.stderr_text.empty() ? completed.stdout_text
														 : completed.stderr_text,
							secret);
		const std::size_t first = detail.find_first_not_of(" \t\n\r\f\v");
		const std::size_t last = detail.find_last_not_of(" \t\n\r\f\v");
		detail = first == std::string::npos ? "" : detail.substr(first, last - first + 1);
		if (detail.size() > 400)
			detail.resize(400);
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' exited " + std::to_string(completed.exit_code) +
			   ": " + detail);
		return json::object();
	}

	std::string text = completed.stdout_text;
	if (spec.output == "jsonl")
	{
		const std::string joined = output_concat_event_text(text, spec.text_path);
		// fall back to the raw stream: a CLI that advertises JSONL may still
		// print one plain object, and losing the reply to a wrong text_path
		// would be worse than trying both
		if (!joined.empty())
			text = joined;
	}

	json handoff;
	odin_error extraction;
	if (!output_extract_object(text, handoff, extraction))
	{
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' returned invalid JSON: " +
			   credential_redact(extraction.message, secret));
		return json::object();
	}
	return handoff;
}
adapter_result adapter_run(const command_spec &spec,
						   const model_profile &profile,
						   const json &request,
						   const project_config &config,
						   odin_error &out_error)
{
	adapter_result result;

	if (command_spec_is_builtin(spec))
	{
		const auto started = std::chrono::steady_clock::now();

		// the same metadata shape a spawned adapter records, so nothing
		// downstream has to know which kind produced the handoff
		result.metadata["command"] = json::array();
		result.metadata["exit_code"] = 0;
		result.metadata["stderr"] = "";

		odin_error kind_error;
		if (spec.type == "mock")
			result.response = adapter_mock(request, profile.model);
		else if (spec.type == "openai-compatible")
			result.response = adapter_openai(spec, profile, request, config, result.metadata,
											 kind_error);
		else if (spec.type == "cli-agent")
			result.response = adapter_cli_agent(spec, profile, request, config, result.metadata,
												kind_error);
		else
			fail(kind_error, error_kind::adapter,
				 "adapter '" + profile.adapter + "' has no implementation for type '" +
				   spec.type + "'");

		const auto elapsed =
		  std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
		result.metadata["model_profile"] = profile.name;
		result.metadata["model"] = profile.model;
		result.metadata["parameter_billions"] = profile.parameter_billions;
		result.metadata["duration_seconds"] = std::round(elapsed.count() * 1e6) / 1e6;

		if (failed(kind_error))
			out_error = kind_error;
		return result;
	}

	subprocess_options options;
	options.command.reserve(spec.command.size());
	for (const std::string &part : spec.command)
	{
		options.command.push_back(adapter_expand(part, profile.model));
	}
	options.working_directory = config.root;
	options.environment = config.environment;
	for (const auto &[name, value] : spec.environment) options.environment[name] = value;
	options.inherit_environment = spec.inherit_environment;
	options.redact_stderr = true;
	options.environment["ODIN_PROJECT_ROOT"] = file_path_utf8(config.root);
	options.input = request.dump();
	options.merge_stderr = false;
	options.timeout_seconds = spec.timeout_seconds;

	const auto started = std::chrono::steady_clock::now();

	odin_error run_error;
	const subprocess_result completed = subprocess_run(options, run_error);
	if (failed(run_error))
	{
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' failed to run: " + run_error.message);
		return result;
	}

	const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
	const double duration = std::round(elapsed.count() * 1e6) / 1e6;

	result.metadata["command"] = options.command;
	result.metadata["exit_code"] = completed.exit_code;
	result.metadata["stderr"] = completed.stderr_text;
	result.metadata["model_profile"] = profile.name;
	result.metadata["model"] = profile.model;
	result.metadata["parameter_billions"] = profile.parameter_billions;
	result.metadata["duration_seconds"] = duration;

	if (completed.exit_code != 0)
	{
		// python strips the captured stderr before interpolating it
		std::string detail = completed.stderr_text;
		const std::size_t first = detail.find_first_not_of(" \t\n\r\f\v");
		const std::size_t last = detail.find_last_not_of(" \t\n\r\f\v");
		detail = first == std::string::npos ? "" : detail.substr(first, last - first + 1);

		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' exited " + std::to_string(completed.exit_code) +
			   ": " + detail);
		return result;
	}

	// note: nlohmann's parse diagnostics differ from python's JSONDecodeError
	// text. accepted divergence, and only reachable via a broken adapter.
	json response = json::parse(completed.stdout_text, nullptr, false);
	if (response.is_discarded())
	{
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' returned invalid JSON");
		return result;
	}
	if (!response.is_object())
	{
		fail(out_error, error_kind::adapter,
			 "adapter '" + profile.adapter + "' must return a JSON object");
		return result;
	}
	subprocess_redact_json(response, options);

	result.response = std::move(response);
	return result;
}
