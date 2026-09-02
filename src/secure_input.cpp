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

namespace
{

// RAII rather than the codebase's usual plain-data style, deliberately: if an
// exception or an early return left echo disabled, the user's terminal would
// stay silently broken after odin exits.
class echo_suppressor
{
  public:
	echo_suppressor()
	{
#ifdef _WIN32
		handle_ = GetStdHandle(STD_INPUT_HANDLE);
		if (handle_ == INVALID_HANDLE_VALUE || !GetConsoleMode(handle_, &original_))
			return;
		suppressed_ = SetConsoleMode(handle_, original_ & ~ENABLE_ECHO_INPUT) != 0;
#else
		if (tcgetattr(STDIN_FILENO, &original_) != 0)
			return;
		termios quiet = original_;
		quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
		// TCSAFLUSH, not TCSANOW: anything typed ahead of the prompt is
		// discarded rather than being echoed by the previous setting.
		suppressed_ = tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet) == 0;
#endif
	}

	~echo_suppressor()
	{
		if (!suppressed_)
			return;
#ifdef _WIN32
		SetConsoleMode(handle_, original_);
#else
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_);
#endif
	}

	bool suppressed() const { return suppressed_; }

	echo_suppressor(const echo_suppressor &) = delete;
	echo_suppressor &operator=(const echo_suppressor &) = delete;

  private:
	bool suppressed_ = false;
#ifdef _WIN32
	HANDLE handle_ = nullptr;
	DWORD original_ = 0;
#else
	termios original_ {};
#endif
};

bool stdin_is_terminal()
{
#ifdef _WIN32
	return _isatty(_fileno(stdin)) != 0;
#else
	return isatty(STDIN_FILENO) != 0;
#endif
}

} // namespace

bool secure_input_read(const std::string &prompt, std::string &out_secret, bool &out_echoed)
{
	if (!stdin_is_terminal())
	{
		// piped or redirected: there is no terminal echo to suppress, and
		// trying to change console mode on a pipe would fail anyway
		out_echoed = false;
		return static_cast<bool>(std::getline(std::cin, out_secret));
	}

	// the prompt goes to stderr so that `odin auth set x > file` does not
	// capture it, and is flushed because it has no newline
	std::fputs(prompt.c_str(), stderr);
	std::fflush(stderr);

	bool read = false;
	{
		const echo_suppressor quiet;
		out_echoed = !quiet.suppressed();
		read = static_cast<bool>(std::getline(std::cin, out_secret));
	}

	// the user's newline was swallowed along with the echo
	std::fputs("\n", stderr);
	std::fflush(stderr);
	return read;
}
