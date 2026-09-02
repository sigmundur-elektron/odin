#include "executable.h"

#include <cstdlib>

namespace fs = std::filesystem;

namespace
{

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

std::vector<std::string> split(const std::string &text, char separator)
{
	std::vector<std::string> parts;
	std::size_t start = 0;
	for (;;)
	{
		const std::size_t at = text.find(separator, start);
		if (at == std::string::npos)
		{
			if (start < text.size())
				parts.push_back(text.substr(start));
			return parts;
		}
		if (at > start)
			parts.push_back(text.substr(start, at - start));
		start = at + 1;
	}
}

bool is_file(const fs::path &path)
{
	std::error_code code;
	return fs::is_regular_file(path, code);
}

} // namespace

const std::vector<std::string> &executable_suffixes()
{
#ifdef _WIN32
	// .cmd before .exe on purpose: npm and every npm-installed CLI ship a .cmd
	// shim, and node ships node.exe. Trying .exe first would resolve a
	// same-named binary in preference to the shim the user actually installed.
	static const std::vector<std::string> suffixes = {".cmd", ".exe", ".bat", ""};
#else
	static const std::vector<std::string> suffixes = {""};
#endif
	return suffixes;
}

std::filesystem::path executable_find_in(const std::string &name,
										 const std::vector<fs::path> &directories)
{
	// an explicit path is used as given rather than searched for
	const fs::path requested(name);
	if (requested.has_parent_path())
	{
		for (const std::string &suffix : executable_suffixes())
		{
			const fs::path candidate = suffix.empty() ? requested
													  : fs::path(name + suffix);
			if (is_file(candidate))
				return candidate;
		}
		return {};
	}

	for (const fs::path &directory : directories)
	{
		if (directory.empty())
			continue;
		for (const std::string &suffix : executable_suffixes())
		{
			const fs::path candidate = directory / (name + suffix);
			if (is_file(candidate))
				return candidate;
		}
	}
	return {};
}

std::filesystem::path executable_find(const std::string &name)
{
#ifdef _WIN32
	constexpr char separator = ';';
#else
	constexpr char separator = ':';
#endif

	std::vector<fs::path> directories;
	for (const std::string &entry : split(environment_value("PATH"), separator))
		directories.emplace_back(entry);

	return executable_find_in(name, directories);
}
