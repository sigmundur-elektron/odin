#include "runtime_paths.h"

#include <cstdint>
#include <cstdlib>
#include <string>

#include "atomic_file.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static fs::path runtime_paths_absolute(const fs::path &path)
{
	std::error_code code;
	fs::path resolved = fs::weakly_canonical(path, code);
	if (!code)
		return resolved;
	resolved = fs::absolute(path, code);
	return code ? path : resolved;
}

static fs::path runtime_paths_executable(const char *argv0)
{
#ifdef _WIN32
	std::wstring buffer(32768, L'\0');
	const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (size > 0 && size < buffer.size())
	{
		buffer.resize(size);
		return runtime_paths_absolute(fs::path(buffer));
	}
#elif defined(__APPLE__)
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::string buffer(size, '\0');
	if (_NSGetExecutablePath(buffer.data(), &size) == 0)
		return runtime_paths_absolute(fs::path(buffer.c_str()));
#else
	std::string buffer(4096, '\0');
	const ssize_t size = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
	if (size > 0)
	{
		buffer.resize(static_cast<std::size_t>(size));
		return runtime_paths_absolute(fs::path(buffer));
	}
#endif

	return runtime_paths_absolute(argv0 == nullptr ? fs::path{} : fs::path(argv0));
}

static fs::path runtime_paths_override()
{
#ifdef _WIN32
	char *value = nullptr;
	std::size_t size = 0;
	if (_dupenv_s(&value, &size, "ODIN_RUNTIME_ROOT") != 0 || value == nullptr)
		return {};
	const fs::path result = value;
	std::free(value);
	return result;
#else
	const char *value = std::getenv("ODIN_RUNTIME_ROOT");
	return value == nullptr ? fs::path{} : fs::path(value);
#endif
}

runtime_paths runtime_paths_resolve(const char *argv0, const fs::path &config_path)
{
	runtime_paths paths;

	const fs::path override_root = runtime_paths_override();
	if (!override_root.empty())
	{
		paths.runtime_root = runtime_paths_absolute(override_root);
	}
	else
	{
		const fs::path executable = runtime_paths_executable(argv0);
		const fs::path installed = runtime_paths_absolute(executable.parent_path() / ODIN_RUNTIME_RELATIVE);

		// the sentinel is a schema, not odin.py. it used to be the latter,
		// which made the installed-layout probe depend on a file the port
		// deletes - and, worse, failed silently: a runtime root that was not
		// found fell through to ODIN_SOURCE_RUNTIME_ROOT, the *build machine's*
		// source directory. that path does not exist on a user's machine, so
		// the real error surfaced later as a confusing missing-definition
		// message pointing at somebody else's disk.
		//
		// handoff.schema.json is the right marker: every run validates against
		// it, so if it is absent nothing downstream can work anyway.
		if (fs::exists(installed / "harness" / "schemas" / "handoff.schema.json"))
			paths.runtime_root = installed;
		else
			paths.runtime_root = runtime_paths_absolute(ODIN_SOURCE_RUNTIME_ROOT);
	}

	paths.config_path = runtime_paths_absolute(config_path);
	paths.project_root = paths.config_path.parent_path();
	return paths;
}

bool runtime_paths_check(const runtime_paths &paths, odin_error &out_error)
{
	const fs::path marker = paths.runtime_root / "harness" / "schemas" / "handoff.schema.json";
	if (fs::exists(marker))
		return true;

	fail(out_error, error_kind::config,
		 "odin's runtime assets were not found under " + file_path_utf8(paths.runtime_root) +
		   ". the installation is incomplete; set ODIN_RUNTIME_ROOT to override.");
	return false;
}
