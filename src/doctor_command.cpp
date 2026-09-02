#include "doctor_command.h"

#include <iostream>

#include "atomic_file.h"
#include "credentials.h"
#include "discovery.h"

namespace fs = std::filesystem;

namespace
{

// pad by character count, not bytes: a model id or path containing anything
// non-ASCII would otherwise misalign every column after it
std::string pad(const std::string &text, std::size_t width)
{
	std::size_t characters = 0;
	for (const char byte : text)
	{
		if ((static_cast<unsigned char>(byte) & 0xC0) != 0x80)
			++characters;
	}
	return characters >= width ? text : text + std::string(width - characters, ' ');
}

std::string status_marker(const std::string &status)
{
	if (status == "ready")
		return "ok";
	if (status == "auth-required")
		return "auth";
	if (status == "error")
		return "err";
	return "--";
}

int render_text(const std::vector<discovered_provider> &providers,
				const fs::path &project_root, bool deep, int ready_count)
{
	std::cout << "Providers reachable from this machine:\n\n";

	for (const discovered_provider &provider : providers)
	{
		std::cout << pad(status_marker(provider.status), 5) << pad(provider.name, 16) << " "
				  << pad(provider.transport, 20) << " " << provider.detail << "\n";

		// eight is enough to recognise what is installed; the full list belongs
		// in --json, which is what a GUI reads
		std::size_t shown = 0;
		for (const discovered_model &model : provider.models)
		{
			if (shown++ >= 8)
			{
				std::cout << "                   ... " << (provider.models.size() - 8)
						  << " more\n";
				break;
			}
			std::string line = "                   " + model.id;
			if (model.parameter_billions.is_number())
				line += "  (" + model.parameter_billions.dump() + "B)";
			std::cout << line << "\n";
		}
	}

	credential_store store;
	credential_store_configure(store, project_root);
	odin_error err;
	const json stored = credential_describe_all(store, err);

	std::cout << "\n";
	if (ready_count == 0)
	{
		std::cout << "Nothing is reachable yet. Three ways forward:\n";
		std::cout << "  1. storing a key      odin auth set openrouter\n";
		std::cout << "  2. a local server     start Ollama or LM Studio\n";
		std::cout << "  3. an agent CLI       odin tools install opencode\n";
	}
	else
	{
		std::cout << "Write configuration from what was observed:\n";
		std::cout << "  odin doctor --emit-config\n";
		if (!deep)
			std::cout << "  odin doctor --deep            also enumerate agent CLI models\n";
	}

	if (!failed(err) && !stored.empty())
	{
		std::cout << "\nStored credentials (values masked):\n";
		for (const json &entry : stored)
		{
			std::cout << "  " << pad(entry.at("name").get<std::string>(), 24) << " "
					  << entry.at("value").get<std::string>() << "\n";
		}
		std::cout << "Reference one from an adapter with --credential <name>.\n";
	}

	return ready_count > 0 ? 0 : 1;
}

} // namespace

int doctor_command_run(const fs::path &project_root, const doctor_options &options)
{
	discovery_options probe;
	probe.deep = options.deep;
	probe.include_hosted = true;
	probe.extra_paths = options.extra_paths;
	probe.project_root = project_root;

	const std::vector<discovered_provider> providers = discovery_run(probe);

	int ready = 0;
	for (const discovered_provider &provider : providers)
	{
		if (provider.status == "ready")
			++ready;
	}

	if (options.emit_config)
	{
		// always 0: an empty config is still a correct answer, and a script
		// piping this into a file should not see a failure for it
		std::cout << discovery_emit_config(providers, 6);
		return 0;
	}

	if (options.as_json)
	{
		json payload;
		payload["providers"] = discovery_to_json(providers);
		payload["ready"] = ready;

		credential_store store;
		credential_store_configure(store, project_root);
		odin_error err;
		const json stored = credential_describe_all(store, err);
		payload["credentials"] = failed(err) ? json::array() : stored;

		std::cout << payload.dump(2, ' ', true, json::error_handler_t::replace) << "\n";
		return ready > 0 ? 0 : 1;
	}

	return render_text(providers, project_root, options.deep, ready);
}
