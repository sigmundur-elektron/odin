#include "tools_command.h"

#include <cstdio>
#include <iostream>

#include "atomic_file.h"
#include "executable.h"
#include "subprocess.h"

namespace fs = std::filesystem;

namespace
{

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

const tool_spec *tools_find(const std::string &name)
{
	for (const tool_spec &spec : tools_known())
	{
		if (spec.name == name)
			return &spec;
	}
	return nullptr;
}

std::string tools_known_names()
{
	std::string names;
	for (const tool_spec &spec : tools_known())
	{
		if (!names.empty())
			names += ", ";
		names += spec.name;
	}
	return names;
}

} // namespace

const std::vector<tool_spec> &tools_known()
{
	// only tools with a known, non-interactive install command are offered
	static const std::vector<tool_spec> known = {
	  {"aider", "aider-chat", "uv", "aider", "Aider coding agent"},
	  {"llm", "llm", "uv", "llm", "Simon Willison's llm CLI, many provider plugins"},
	  {"opencode", "opencode-ai", "npm", "opencode",
	   "OpenCode CLI; reuses an existing OpenCode login if present"},
	};
	return known;
}

fs::path tools_installed_path(const fs::path &project_root, const tool_spec &spec)
{
	const fs::path target = project_root / ".odin" / "tools" / spec.name;

	// every layout is probed on every platform: which one a package manager
	// produces depends on the manager and the package, not on the OS, so
	// branching on the OS here would miss the common cases.
	const fs::path candidates[] = {
	  target / "node_modules" / ".bin" / (spec.binary + ".cmd"),
	  target / "node_modules" / ".bin" / spec.binary,
	  target / "Scripts" / (spec.binary + ".exe"),
	  target / "bin" / spec.binary,
	};

	std::error_code code;
	for (const fs::path &candidate : candidates)
	{
		if (fs::is_regular_file(candidate, code))
			return candidate;
	}
	return {};
}

int tools_command_run(const fs::path &project_root, const tools_options &options)
{
	if (options.subcommand == "list")
	{
		for (const tool_spec &spec : tools_known())
		{
			const fs::path found = tools_installed_path(project_root, spec);
			std::cout << pad(spec.name, 12) << " " << pad(spec.manager, 5) << " "
					  << (found.empty() ? "not installed" : file_path_utf8(found)) << "\n";
			std::cout << "             " << spec.description << "\n";
		}
		std::cout << "\nOdin installs nothing automatically. Install explicitly with:\n";
		std::cout << "  odin tools install <name>\n";
		return 0;
	}

	if (options.subcommand != "install")
	{
		std::cerr << "odin: unknown tools subcommand '" << options.subcommand << "'\n";
		return 2;
	}

	const tool_spec *spec = tools_find(options.name);
	if (spec == nullptr)
	{
		std::cerr << "odin: unknown tool '" << options.name << "'. Known: " << tools_known_names()
				  << "\n";
		return 2;
	}

	const fs::path target = project_root / ".odin" / "tools" / spec->name;
	std::error_code code;
	fs::create_directories(target, code);
	if (code)
	{
		std::cerr << "odin: could not create " << file_path_utf8(target) << ": " << code.message()
				  << "\n";
		return 2;
	}

	subprocess_options run;
	if (spec->manager == "npm")
	{
		if (executable_find("npm").empty())
		{
			std::cerr << "odin: npm is required to install this tool but was not found on PATH\n";
			return 2;
		}
		run.command = {"npm", "install", "--prefix", file_path_utf8(target), spec->package};
	}
	else
	{
		// uv rather than pip: uv provides its own interpreter, so installing a
		// Python-implemented tool does not reintroduce a Python dependency for
		// Odin itself.
		if (executable_find("uv").empty())
		{
			std::cerr << "odin: uv is required to install this tool but was not found on PATH.\n"
					  << "      see https://docs.astral.sh/uv/ for installation.\n";
			return 2;
		}
		run.command = {"uv", "pip", "install", "--target", file_path_utf8(target), spec->package};
	}

	run.working_directory = project_root;
	run.timeout_seconds = options.timeout_seconds;
	run.merge_stderr = false;

	odin_error err;
	const subprocess_result completed = subprocess_run(run, err);
	if (failed(err))
	{
		std::cerr << "odin: install failed: " << err.message << "\n";
		return 2;
	}
	if (completed.exit_code != 0)
	{
		std::string detail = completed.stderr_text.empty() ? completed.stdout_text
														   : completed.stderr_text;
		if (detail.size() > 400)
			detail.resize(400);
		std::cerr << "odin: " << python_list_repr(run.command) << " exited "
				  << completed.exit_code << ": " << detail << "\n";
		return 2;
	}

	// the manager reported success; confirm the binary is actually where the
	// rest of Odin will look for it, rather than trusting the exit code
	const fs::path resolved = tools_installed_path(project_root, *spec);
	if (resolved.empty())
	{
		std::cerr << "odin: '" << spec->name << "' installed but its binary was not found under "
				  << file_path_utf8(target) << "\n";
		return 2;
	}

	std::cout << "installed '" << spec->name << "' at " << file_path_utf8(resolved) << "\n";
	std::cout << "It is inside .odin/tools, which is gitignored and searched by `doctor`.\n";
	std::cout << "Confirm with: odin doctor --deep\n";
	return 0;
}
