#include "adapter.h"

#include <chrono>
#include <cmath>

#include "atomic_file.h"

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

adapter_result adapter_run(const command_spec &spec,
						   const model_profile &profile,
						   const json &request,
						   const project_config &config,
						   odin_error &out_error)
{
	adapter_result result;

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
