#include "atomic_file.h"
#include "config.h"
#include "json_io.h"

#include <cstdio>
#include <string>

// placeholder entry point. argument parsing and the real commands land in P6;
// until then this loads odin.toml and prints what it found, which is enough to
// smoke-test config_load against a real project.
int main(int argc, char **argv)
{
	const std::string config_path = argc > 1 ? argv[1] : "odin.toml";

	odin_error err;
	const project_config config = config_load(config_path, err);
	if (failed(err))
	{
		std::fprintf(stderr, "odin: %s\n", err.message.c_str());
		return 2;
	}

	std::printf("root       %s\n", file_path_utf8(config.root).c_str());
	std::printf("state_dir  %s\n", file_path_utf8(config.state_dir).c_str());
	std::printf("max_total_transitions %d\n", config.max_total_transitions);
	std::printf("stage_on_success      %s\n", config.stage_on_success ? "true" : "false");

	for (const auto &[name, spec] : config.adapters)
	{
		std::printf("adapter    %-12s %zu arg(s), timeout %ds\n",
					name.c_str(), spec.command.size(), spec.timeout_seconds);
	}
	for (const auto &[name, spec] : config.gates)
	{
		std::printf("gate       %-12s %zu arg(s), timeout %ds\n",
					name.c_str(), spec.command.size(), spec.timeout_seconds);
	}
	for (const auto &[name, profile] : config.models)
	{
		std::printf("model      %-12s adapter=%s model=%s\n",
					name.c_str(), profile.adapter.c_str(), profile.model.c_str());
	}
	for (const auto &[role, profile] : config.routing)
	{
		std::printf("routing    %-12s -> %s\n", role.c_str(), profile.c_str());
	}
	return 0;
}
