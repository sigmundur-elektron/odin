#include "secure_input.h"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

// The saved terminal state, so it can be put back exactly as it was.
//
// `suppressed` is what says whether there is anything to restore: querying the
// mode can fail (stdin is not a console, the handle is closed), and restoring
// state that was never captured would push garbage into the terminal.
struct echo_state
{
#ifdef _WIN32
	HANDLE handle = nullptr;
	DWORD original = 0;
#else
	termios original {};
#endif
	bool suppressed = false;
};

static void echo_disable(echo_state &out_state)
{
#ifdef _WIN32
	out_state.handle = GetStdHandle(STD_INPUT_HANDLE);
	if (out_state.handle == INVALID_HANDLE_VALUE || out_state.handle == nullptr)
		return;
	if (!GetConsoleMode(out_state.handle, &out_state.original))
		return;
	out_state.suppressed =
	  SetConsoleMode(out_state.handle, out_state.original & ~ENABLE_ECHO_INPUT) != 0;
#else
	if (tcgetattr(STDIN_FILENO, &out_state.original) != 0)
		return;
	termios quiet = out_state.original;
	quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
	// TCSAFLUSH, not TCSANOW: anything typed ahead of the prompt is discarded
	// rather than being echoed under the previous setting.
	out_state.suppressed = tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0;
#endif
}

// Must run on every path out of the read, including the failure ones. Leaving
// echo off would silently break the user's terminal after odin exits.
static void echo_restore(const echo_state &state)
{
	if (!state.suppressed)
		return;
#ifdef _WIN32
	SetConsoleMode(state.handle, state.original);
#else
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &state.original);
#endif
}

static bool stdin_is_terminal()
{
#ifdef _WIN32
	return _isatty(_fileno(stdin)) != 0;
#else
	return isatty(STDIN_FILENO) != 0;
#endif
}

bool secure_input_read(const std::string &prompt, std::string &out_secret, bool &out_echoed)
{
	if (!stdin_is_terminal())
	{
		// piped or redirected: there is no terminal echo to suppress, and
		// changing console mode on a pipe would fail anyway
		out_echoed = false;
		return static_cast<bool>(std::getline(std::cin, out_secret));
	}

	// the prompt goes to stderr so `odin auth set x > file` does not capture it,
	// and is flushed because it carries no newline
	std::fputs(prompt.c_str(), stderr);
	std::fflush(stderr);

	echo_state state;
	echo_disable(state);
	out_echoed = !state.suppressed;

	const bool read = static_cast<bool>(std::getline(std::cin, out_secret));

	echo_restore(state);

	// the user's own newline was swallowed along with the echo
	std::fputs("\n", stderr);
	std::fflush(stderr);
	return read;
}
