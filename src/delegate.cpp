#include "delegate.h"

#include <vector>

#include "atomic_file.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// CreateProcessW rather than the CRT's _wspawnvp: it states the handle
// inheritance explicitly instead of relying on convention, which is the whole
// point of this file.
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

bool delegate_owns(const std::string &command)
{
	return command == "doctor" || command == "tools" || command == "auth";
}

#ifdef _WIN32

static std::wstring widen(const std::string &utf8)
{
	if (utf8.empty())
		return {};
	const int size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
										 nullptr, 0);
	std::wstring wide(static_cast<std::size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), size);
	return wide;
}

// CreateProcessW takes one command line, so each argument has to be quoted the
// way the CRT will later split it: backslashes are only special immediately
// before a quote.
static void append_argument(std::wstring &line, const std::wstring &argument)
{
	if (!line.empty())
		line.push_back(L' ');

	const bool needs_quotes = argument.empty() ||
							  argument.find_first_of(L" \t\n\v\"") != std::wstring::npos;
	if (!needs_quotes)
	{
		line += argument;
		return;
	}

	line.push_back(L'"');
	for (std::size_t at = 0; at < argument.size(); ++at)
	{
		std::size_t slashes = 0;
		while (at < argument.size() && argument[at] == L'\\')
		{
			++slashes;
			++at;
		}

		if (at == argument.size())
		{
			line.append(slashes * 2, L'\\');
			break;
		}
		if (argument[at] == L'"')
		{
			line.append(slashes * 2 + 1, L'\\');
		}
		else
		{
			line.append(slashes, L'\\');
		}
		line.push_back(argument[at]);
	}
	line.push_back(L'"');
}

#endif

int delegate_to_python(const std::string &interpreter,
					   const std::filesystem::path &runtime_root,
					   const std::filesystem::path &project_root,
					   const std::vector<std::string> &argv,
					   odin_error &out_error)
{
	// odin.py, not "-m harness.cli": harness/cli.py defines main() but has no
	// __main__ block, so running it as a module would silently do nothing.
	std::vector<std::string> command;
	command.reserve(argv.size() + 2);
	command.push_back(interpreter);
	command.push_back(file_path_utf8(runtime_root / "odin.py"));
	for (const std::string &part : argv)
		command.push_back(part);

	// reproc is deliberately not used here. it exists to CAPTURE a child, and
	// its parent-redirect path is subtle enough that stdout can be lost. what
	// delegation needs is the opposite and much simpler: hand the child our own
	// stdin, stdout and stderr and wait. that is exactly what the platform's
	// spawn-and-wait primitive already does, and it is what keeps getpass from
	// echoing and lets npm render progress live.

#ifdef _WIN32
	std::wstring line;
	for (const std::string &part : command)
		append_argument(line, widen(part));

	const std::wstring working_directory = project_root.wstring();

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	// no STARTF_USESTDHANDLES and no attribute list: the child simply inherits
	// our console or redirection, which is the entire point of delegating.
	PROCESS_INFORMATION created = {};

	std::vector<wchar_t> mutable_line(line.begin(), line.end());
	mutable_line.push_back(L'\0');

	const BOOL started = CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, 0,
										nullptr,
										working_directory.empty() ? nullptr
																  : working_directory.c_str(),
										&startup, &created);
	if (!started)
	{
		fail(out_error, error_kind::config,
			 "could not run '" + interpreter +
			   "'. is python on PATH? set ODIN_PYTHON to choose a different interpreter.");
		return 2;
	}

	WaitForSingleObject(created.hProcess, INFINITE);

	DWORD status = 2;
	GetExitCodeProcess(created.hProcess, &status);
	CloseHandle(created.hThread);
	CloseHandle(created.hProcess);
	return static_cast<int>(status);
#else
	std::vector<char *> raw;
	raw.reserve(command.size() + 1);
	for (std::string &part : command)
		raw.push_back(part.data());
	raw.push_back(nullptr);

	const pid_t child = fork();
	if (child < 0)
	{
		fail(out_error, error_kind::config, "could not fork to run '" + interpreter + "'");
		return 2;
	}
	if (child == 0)
	{
		if (chdir(file_path_utf8(project_root).c_str()) != 0)
			_exit(127);
		execvp(raw[0], raw.data());
		_exit(127); // only reached when exec failed
	}

	int status = 0;
	if (waitpid(child, &status, 0) < 0)
	{
		fail(out_error, error_kind::config, "python cli did not exit cleanly");
		return 2;
	}
	if (WIFEXITED(status))
	{
		const int code = WEXITSTATUS(status);
		if (code == 127)
		{
			fail(out_error, error_kind::config,
				 "could not run '" + interpreter + "'. is python on PATH? set ODIN_PYTHON to "
												   "choose a different interpreter.");
			return 2;
		}
		return code;
	}
	return 2;
#endif
}
