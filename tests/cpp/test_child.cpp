// A deterministic child process for the native test suite.
//
// The C++ tests exercise a process boundary - pipe buffering, stdin/stdout
// interleaving, timeouts, process-tree kills, environment scoping - and there is
// nothing useful to mock about it, so a real child has to exist. That child used
// to be `python -c "<snippet>"`, which quietly made a Python interpreter a
// requirement for `ctest` long after the product stopped needing one.
//
// This binary replaces those snippets. It is deliberately dumb: a list of
// directives evaluated left to right, so a test reads as data rather than as an
// embedded program in another language.
//
//   out:<text>          write <text> to stdout, verbatim, no newline
//   err:<text>          write <text> to stderr
//   env:<NAME>          write $NAME to stdout, empty when unset
//   envor:<NAME>=<alt>  write $NAME to stdout, <alt> when unset
//   errenv:<NAME>       write $NAME to stderr
//   errjson:<NAME>      write $NAME to stderr as an ASCII-escaped JSON string
//   has:<NAME>          write "True" or "False"
//   cwd                 write the working directory to stdout
//   cat                 copy stdin to stdout until EOF
//   sleep:<seconds>     sleep
//   touch:<path>        create an empty file
//   spawn:<sec>:<path>  detach a grandchild that sleeps then creates <path>
//   nop:<anything>      ignored; exists so a test can pad argv
//   art:<key>=<text>    add a literal handoff artifact
//   artenv:<key>=<NAME> add a handoff artifact from $NAME, null when unset
//   handoff             emit an approved handoff/v1 object on exit
//   exit:<code>         stop immediately with <code>
//
// Two whole-program modes replace the fixtures that needed real logic:
//
//   mock [--model M]                 the deterministic agent formerly in
//                                    scripts/mock_agent.py
//   agent-changed <agent> <json>     an approved handoff whose changed_files is
//                                    <json> for <agent> and [] for every other

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

using json = nlohmann::json;

// ------------------------------------------------------------------ output

// stdout is put in binary mode on Windows so that a test asserting exact bytes
// is not defeated by the CRT turning \n into \r\n behind it.
static void write_out(const std::string &text)
{
	std::fwrite(text.data(), 1, text.size(), stdout);
	std::fflush(stdout);
}

static void write_err(const std::string &text)
{
	std::fwrite(text.data(), 1, text.size(), stderr);
	std::fflush(stderr);
}

static std::string read_all_stdin()
{
	std::string text;
	char buffer[8192];
	std::size_t count = 0;
	while ((count = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0)
		text.append(buffer, count);
	return text;
}

// ------------------------------------------------------------- environment

// returns false when the variable is absent, which the caller distinguishes from
// an empty value: `artenv` emits null for the former and "" for the latter.
//
// on windows this deliberately goes through the WIDE environment and converts
// to utf-8 by hand, rather than using getenv/_dupenv_s. odin hands children a
// utf-8 environment block, which reproc widens with CP_UTF8; the narrow CRT
// accessors would convert that back down through the ANSI codepage instead, so
// a non-ascii secret would arrive here as different bytes than the parent holds
// and would no longer match on the redaction path.
static bool env_lookup(const std::string &name, std::string &out_value)
{
#ifdef _WIN32
	const int wide_name_size =
	  MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
	std::wstring wide_name(static_cast<std::size_t>(wide_name_size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide_name.data(),
						wide_name_size);

	const DWORD needed = GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
	if (needed == 0)
		return false; // absent; an empty value still reports a length of 1

	std::wstring wide_value(needed, L'\0');
	const DWORD written = GetEnvironmentVariableW(wide_name.c_str(), wide_value.data(), needed);
	wide_value.resize(written);

	if (wide_value.empty())
	{
		out_value.clear();
		return true;
	}

	const int size = WideCharToMultiByte(CP_UTF8, 0, wide_value.data(),
										 static_cast<int>(wide_value.size()), nullptr, 0, nullptr,
										 nullptr);
	out_value.assign(static_cast<std::size_t>(size), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide_value.data(), static_cast<int>(wide_value.size()),
						out_value.data(), size, nullptr, nullptr);
	return true;
#else
	const char *value = std::getenv(name.c_str());
	if (value == nullptr)
		return false;
	out_value.assign(value);
	return true;
#endif
}

// ------------------------------------------------------------------ splits

// split on the FIRST separator only. paths carry colons on Windows and artifact
// values carry '=', so anything greedier would corrupt them.
static bool split_once(const std::string &text, char separator, std::string &out_left,
					   std::string &out_right)
{
	const std::size_t at = text.find(separator);
	if (at == std::string::npos)
		return false;
	out_left = text.substr(0, at);
	out_right = text.substr(at + 1);
	return true;
}

static bool directive(const std::string &argument, const char *name, std::string &out_value)
{
	const std::size_t length = std::strlen(name);
	if (argument.size() < length + 1)
		return false;
	if (argument.compare(0, length, name) != 0 || argument[length] != ':')
		return false;
	out_value = argument.substr(length + 1);
	return true;
}

// ----------------------------------------------------------------- spawning

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

static void append_quoted(std::wstring &line, const std::wstring &argument)
{
	if (!line.empty())
		line.push_back(L' ');
	line.push_back(L'"');
	for (const wchar_t character : argument)
	{
		if (character == L'"' || character == L'\\')
			line.push_back(L'\\');
		line.push_back(character);
	}
	line.push_back(L'"');
}
#endif

// start a detached copy of this executable that sleeps and then creates a file.
//
// the test that uses this asserts the file never appears: killing only the
// direct child would strand the grandchild, and the marker is how that leak
// becomes visible.
static void spawn_grandchild(const std::string &self, int seconds, const std::string &marker)
{
	const std::string sleep_directive = "sleep:" + std::to_string(seconds);
	const std::string touch_directive = "touch:" + marker;

#ifdef _WIN32
	std::wstring line;
	append_quoted(line, widen(self));
	append_quoted(line, widen(sleep_directive));
	append_quoted(line, widen(touch_directive));

	STARTUPINFOW startup = {};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION created = {};

	std::vector<wchar_t> mutable_line(line.begin(), line.end());
	mutable_line.push_back(L'\0');

	if (CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
					   &startup, &created))
	{
		CloseHandle(created.hThread);
		CloseHandle(created.hProcess);
	}
#else
	const pid_t child = fork();
	if (child != 0)
		return; // parent, including the fork-failed case

	std::string self_copy = self;
	std::string sleep_copy = sleep_directive;
	std::string touch_copy = touch_directive;
	char *raw[] = {self_copy.data(), sleep_copy.data(), touch_copy.data(), nullptr};
	execv(self_copy.c_str(), raw);
	_exit(127);
#endif
}

// --------------------------------------------------------------- whole modes

// the deterministic agent that used to live in scripts/mock_agent.py. it is test
// infrastructure that proves workflow mechanics and contracts; it is not a model
// and must never be read as implementation evidence.
static int run_mock(const std::vector<std::string> &arguments)
{
	std::string model = "mock";
	for (std::size_t at = 0; at < arguments.size(); ++at)
	{
		if (arguments[at] == "--model" && at + 1 < arguments.size())
			model = arguments[++at];
	}

	std::string input = read_all_stdin();
	const json request = json::parse(input, nullptr, false);
	if (request.is_discarded() || !request.is_object())
	{
		write_err("mock: stdin was not a JSON object\n");
		return 1;
	}

	const std::string agent = request.at("agent").at("id").get<std::string>();

	json artifacts = json::object();
	if (agent == "analyst")
	{
		artifacts["requirements"] = json::array({request.at("task").at("request")});
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
	write_out(handoff.dump());
	return 0;
}

// an approved handoff that reports changed_files only for one named agent. the
// staging tests need to control that list precisely, including making it invalid.
static int run_agent_changed(const std::vector<std::string> &arguments)
{
	if (arguments.size() < 2)
	{
		write_err("agent-changed: expected <agent> <json-array>\n");
		return 2;
	}

	std::string input = read_all_stdin();
	const json request = json::parse(input, nullptr, false);
	if (request.is_discarded() || !request.is_object())
	{
		write_err("agent-changed: stdin was not a JSON object\n");
		return 1;
	}

	const std::string agent = request.at("agent").at("id").get<std::string>();
	json changed = json::array();
	if (agent == arguments[0])
	{
		changed = json::parse(arguments[1], nullptr, false);
		if (changed.is_discarded())
		{
			write_err("agent-changed: argument was not valid JSON\n");
			return 2;
		}
	}

	json handoff;
	handoff["status"] = "approved";
	handoff["summary"] = "ok";
	handoff["artifacts"] = json{{"changed_files", changed}};
	handoff["findings"] = json::array();
	write_out(handoff.dump());
	return 0;
}

// ---------------------------------------------------------------------- main

int main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
	_setmode(_fileno(stderr), _O_BINARY);
#endif

	const std::string self = argc > 0 ? argv[0] : "";
	std::vector<std::string> arguments(argv + (argc > 0 ? 1 : 0), argv + argc);

	if (!arguments.empty() && arguments[0] == "mock")
		return run_mock({arguments.begin() + 1, arguments.end()});
	if (!arguments.empty() && arguments[0] == "agent-changed")
		return run_agent_changed({arguments.begin() + 1, arguments.end()});

	json artifacts = json::object();
	bool emit_handoff = false;
	std::string value;

	for (const std::string &argument : arguments)
	{
		if (argument == "cwd")
		{
			write_out(std::filesystem::current_path().string());
		}
		else if (argument == "cat")
		{
			// block-wise rather than line-wise: one test feeds half a megabyte
			// through to prove neither pipe deadlocks, and it contains no
			// newlines at all.
			char buffer[8192];
			std::size_t count = 0;
			while ((count = std::fread(buffer, 1, sizeof(buffer), stdin)) > 0)
				std::fwrite(buffer, 1, count, stdout);
			std::fflush(stdout);
		}
		else if (argument == "handoff")
		{
			emit_handoff = true;
		}
		else if (directive(argument, "out", value))
		{
			write_out(value);
		}
		else if (directive(argument, "err", value))
		{
			write_err(value);
		}
		else if (directive(argument, "env", value))
		{
			std::string found;
			env_lookup(value, found);
			write_out(found);
		}
		else if (directive(argument, "envor", value))
		{
			std::string name;
			std::string fallback;
			split_once(value, '=', name, fallback);
			std::string found;
			write_out(env_lookup(name, found) ? found : fallback);
		}
		else if (directive(argument, "errenv", value))
		{
			std::string found;
			env_lookup(value, found);
			write_err(found);
		}
		else if (directive(argument, "errjson", value))
		{
			// ensure_ascii, so a non-ASCII secret reaches stderr in its
			// \uXXXX form. that is the shape redaction has to survive.
			//
			// error_handler_t::replace because a test may hand this an
			// arbitrary byte string: nlohmann's default is to throw on invalid
			// utf-8, and a helper that aborts turns a clear assertion failure
			// into an unexplained exit code 3.
			std::string found;
			env_lookup(value, found);
			write_err(json(found).dump(-1, ' ', true, json::error_handler_t::replace));
		}
		else if (directive(argument, "has", value))
		{
			std::string found;
			write_out(env_lookup(value, found) ? "True" : "False");
		}
		else if (directive(argument, "sleep", value))
		{
			std::this_thread::sleep_for(std::chrono::seconds(std::stoi(value)));
		}
		else if (directive(argument, "touch", value))
		{
			std::ofstream stream(value, std::ios::binary | std::ios::trunc);
		}
		else if (directive(argument, "spawn", value))
		{
			std::string seconds;
			std::string marker;
			split_once(value, ':', seconds, marker);
			spawn_grandchild(self, std::stoi(seconds), marker);
		}
		else if (directive(argument, "art", value))
		{
			std::string key;
			std::string literal;
			split_once(value, '=', key, literal);
			artifacts[key] = literal;
		}
		else if (directive(argument, "artenv", value))
		{
			std::string key;
			std::string name;
			split_once(value, '=', key, name);
			std::string found;
			if (env_lookup(name, found))
				artifacts[key] = found;
			else
				artifacts[key] = nullptr;
		}
		else if (directive(argument, "nop", value))
		{
			// padding, so a test can assert on an argument's index
		}
		else if (directive(argument, "exit", value))
		{
			return std::stoi(value);
		}
		else
		{
			write_err("test_child: unknown directive '" + argument + "'\n");
			return 2;
		}
	}

	if (emit_handoff)
	{
		json handoff;
		handoff["status"] = "approved";
		handoff["summary"] = "ok";
		handoff["artifacts"] = artifacts;
		handoff["findings"] = json::array();
		write_out(handoff.dump());
	}
	return 0;
}
