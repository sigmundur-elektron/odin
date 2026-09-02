#include "subprocess.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include <reproc++/reproc.hpp>

#include "atomic_file.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

constexpr std::size_t read_chunk = 4096;

static std::string environment_value(const char *name)
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

// windows environment variable names are case-insensitive, so an overlay of
// "path" has to displace an inherited "PATH" rather than sit beside it. posix
// names are case-sensitive and std::map's own ordering already settles it, which
// is why both this and its only call site are windows-only.
#ifdef _WIN32
static bool environment_same_name(const std::string &left, const std::string &right)
{
	if (left.size() != right.size())
		return false;
	for (std::size_t i = 0; i < left.size(); ++i)
	{
		if (std::tolower(static_cast<unsigned char>(left[i])) !=
			std::tolower(static_cast<unsigned char>(right[i])))
			return false;
	}
	return true;
}
#endif

static bool environment_set(std::map<std::string, std::string> &environment,
							const std::string &name,
							const std::string &value,
							odin_error &out_error)
{
	if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos ||
		value.find('\0') != std::string::npos)
	{
		fail(out_error, error_kind::config, "invalid child environment entry '" + name + "'");
		return false;
	}
#ifdef _WIN32
	for (auto entry = environment.begin(); entry != environment.end();)
	{
		if (environment_same_name(entry->first, name))
			entry = environment.erase(entry);
		else
			++entry;
	}
#endif
	environment[name] = value;
	return true;
}

bool subprocess_environment_build(const std::vector<std::string> &inherit,
								  const std::map<std::string, std::string> &configured,
								  std::map<std::string, std::string> &out_environment,
								  odin_error &out_error)
{
	static const char *baseline[] = {
	  "PATH",
	  "HOME",
	  "TEMP",
	  "TMP",
	  "TMPDIR",
	  "LANG",
	  "LANGUAGE",
	  "LC_ALL",
	  "LC_CTYPE",
	  "LC_MESSAGES",
	  "LC_COLLATE",
	  "LC_MONETARY",
	  "LC_NUMERIC",
	  "LC_TIME",
	  "TZ",
	  "SSL_CERT_FILE",
	  "SSL_CERT_DIR",
#ifdef _WIN32
	  "SystemRoot",
	  "WINDIR",
	  "COMSPEC",
	  "PATHEXT",
	  "USERPROFILE",
	  "HOMEDRIVE",
	  "HOMEPATH",
	  "APPDATA",
	  "LOCALAPPDATA",
#endif
	};

	for (const char *name : baseline)
	{
		const std::string value = environment_value(name);
		if (!value.empty() && !environment_set(out_environment, name, value, out_error))
			return false;
	}
	for (const std::string &name : inherit)
	{
		const std::string value = environment_value(name.c_str());
		if (!value.empty() && !environment_set(out_environment, name, value, out_error))
			return false;
	}
	for (const auto &[name, value] : configured)
	{
		if (!environment_set(out_environment, name, value, out_error))
			return false;
	}
	return true;
}

static void environment_redact(std::string &text, const subprocess_options &options)
{
	std::vector<std::string> values;
	for (const std::string &name : options.inherit_environment)
	{
		const std::string value = environment_value(name.c_str());
		if (!value.empty())
			values.push_back(value);
	}
	for (const auto &[name, value] : options.environment)
	{
		std::string upper = name;
		std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char value) {
			return static_cast<char>(std::toupper(value));
		});
		if (upper.find("KEY") != std::string::npos || upper.find("TOKEN") != std::string::npos ||
			upper.find("SECRET") != std::string::npos || upper.find("PASSWORD") != std::string::npos ||
			upper.find("CREDENTIAL") != std::string::npos)
			values.push_back(value);
	}
	for (const std::string &value : values)
	{
		if (value.empty())
			continue;
		std::vector<std::string> forms = {value};
		const std::string serialized = json(value).dump();
		if (serialized.size() >= 2)
			forms.push_back(serialized.substr(1, serialized.size() - 2));
		const std::string ascii_serialized = json(value).dump(-1, ' ', true);
		if (ascii_serialized.size() >= 2)
			forms.push_back(ascii_serialized.substr(1, ascii_serialized.size() - 2));
		std::sort(forms.begin(), forms.end(), [](const std::string &left, const std::string &right) {
			return left.size() > right.size();
		});
		for (const std::string &form : forms)
		{
			std::size_t at = 0;
			while (!form.empty() && (at = text.find(form, at)) != std::string::npos)
			{
				text.replace(at, form.size(), "[REDACTED]");
				at += 10;
			}
		}
	}
}

static std::vector<std::string> environment_secret_values(const subprocess_options &options)
{
	std::vector<std::string> values;
	for (const std::string &name : options.inherit_environment)
	{
		const std::string value = environment_value(name.c_str());
		if (!value.empty())
			values.push_back(value);
	}
	for (const auto &[name, value] : options.environment)
	{
		std::string upper = name;
		std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char item) {
			return static_cast<char>(std::toupper(item));
		});
		if (upper.find("KEY") != std::string::npos || upper.find("TOKEN") != std::string::npos ||
			upper.find("SECRET") != std::string::npos || upper.find("PASSWORD") != std::string::npos ||
			upper.find("CREDENTIAL") != std::string::npos)
			values.push_back(value);
	}
	return values;
}

void subprocess_redact_json(json &value, const subprocess_options &options)
{
	const std::vector<std::string> secrets = environment_secret_values(options);
	const auto redact = [&](auto &&self, json &item) -> void {
		if (item.is_object())
		{
			json redacted = json::object();
			for (auto entry = item.begin(); entry != item.end(); ++entry)
			{
				std::string key = entry.key();
				for (const std::string &secret : secrets)
				{
					std::size_t at = 0;
					while (!secret.empty() && (at = key.find(secret, at)) != std::string::npos)
					{
						key.replace(at, secret.size(), "[REDACTED]");
						at += 10;
					}
				}
				json value = entry.value();
				self(self, value);
				redacted[key] = std::move(value);
			}
			item = std::move(redacted);
		}
		else if (item.is_array())
		{
			for (json &entry : item) self(self, entry);
		}
		else if (item.is_string())
		{
			std::string text = item.get<std::string>();
			for (const std::string &secret : secrets)
			{
				if (!secret.empty() && text.find(secret) != std::string::npos)
				{
					std::size_t at = 0;
					while ((at = text.find(secret, at)) != std::string::npos)
					{
						text.replace(at, secret.size(), "[REDACTED]");
						at += 10;
					}
				}
			}
			item = text;
		}
	};
	redact(redact, value);
}

// ---------------------------------------------------------------- utf-8

static const char replacement[] = "\xEF\xBF\xBD"; // u+fffd

// unicode table 3-7: what a well formed sequence may contain. the second byte's
// range depends on the lead, which is what rules out overlong forms, surrogates
// and anything past u+10ffff without ever decoding the code point.
struct utf8_shape
{
	int length = 0; // 0 marks a byte that cannot begin a sequence
	unsigned char second_low = 0x80;
	unsigned char second_high = 0xBF;
};

static utf8_shape utf8_shape_of(unsigned char lead)
{
	if (lead <= 0x7F)
		return {1, 0, 0};
	if (lead >= 0xC2 && lead <= 0xDF)
		return {2, 0x80, 0xBF};
	if (lead == 0xE0)
		return {3, 0xA0, 0xBF}; // 0x80..0x9F would be overlong
	if (lead >= 0xE1 && lead <= 0xEC)
		return {3, 0x80, 0xBF};
	if (lead == 0xED)
		return {3, 0x80, 0x9F}; // 0xA0.. would be a surrogate
	if (lead >= 0xEE && lead <= 0xEF)
		return {3, 0x80, 0xBF};
	if (lead == 0xF0)
		return {4, 0x90, 0xBF}; // 0x80..0x8F would be overlong
	if (lead >= 0xF1 && lead <= 0xF3)
		return {4, 0x80, 0xBF};
	if (lead == 0xF4)
		return {4, 0x80, 0x8F}; // 0x90.. would exceed u+10ffff
	return {};					// 0x80..0xC1 and 0xF5..0xFF
}

std::string utf8_sanitize(const std::string &bytes)
{
	std::string out;
	out.reserve(bytes.size());

	std::size_t at = 0;
	while (at < bytes.size())
	{
		const unsigned char lead = static_cast<unsigned char>(bytes[at]);
		const utf8_shape shape = utf8_shape_of(lead);

		if (shape.length == 1)
		{
			out.push_back(bytes[at++]);
			continue;
		}
		if (shape.length == 0)
		{
			out.append(replacement);
			++at;
			continue;
		}

		// consume only the bytes that are actually well formed - the "maximal
		// subpart" rule Unicode specifies. it is why b"\xe0\x80\xaf" yields three
		// replacements and not one: 0x80 is outside 0xE0's permitted range, so
		// the lead stands alone and the two stray bytes are separate errors.
		int consumed = 1;
		bool valid = true;
		while (consumed < shape.length)
		{
			if (at + static_cast<std::size_t>(consumed) >= bytes.size())
			{
				valid = false;
				break;
			}
			const unsigned char next =
			  static_cast<unsigned char>(bytes[at + static_cast<std::size_t>(consumed)]);
			const unsigned char low = consumed == 1 ? shape.second_low : 0x80;
			const unsigned char high = consumed == 1 ? shape.second_high : 0xBF;
			if (next < low || next > high)
			{
				valid = false;
				break;
			}
			++consumed;
		}

		if (valid)
		{
			out.append(bytes, at, static_cast<std::size_t>(shape.length));
			at += static_cast<std::size_t>(shape.length);
		}
		else
		{
			out.append(replacement);
			at += static_cast<std::size_t>(consumed);
		}
	}
	return out;
}

std::string command_repr(const std::vector<std::string> &values)
{
	std::string out = "[";
	for (std::size_t i = 0; i < values.size(); ++i)
	{
		if (i > 0)
			out += ", ";
		// single quotes, switching to double when
		// the value itself contains one.
		const bool has_single = values[i].find('\'') != std::string::npos;
		const char quote = has_single ? '"' : '\'';
		out.push_back(quote);
		out += values[i];
		out.push_back(quote);
	}
	out.push_back(']');
	return out;
}

// ------------------------------------------------------------- sigpipe

void subprocess_ignore_sigpipe()
{
#ifndef _WIN32
	static std::once_flag once;
	std::call_once(once, [] {
		// leave a disposition an embedder chose alone. odin_core is meant to be
		// linked into the gui as well as the cli, and this is process-global.
		struct sigaction current = {};
		if (sigaction(SIGPIPE, nullptr, &current) != 0)
			return;
		if ((current.sa_flags & SA_SIGINFO) != 0 || current.sa_handler != SIG_DFL)
			return;

		struct sigaction ignore = {};
		ignore.sa_handler = SIG_IGN;
		sigemptyset(&ignore.sa_mask);
		sigaction(SIGPIPE, &ignore, nullptr);
	});
#endif
}

// ------------------------------------------------------------ job objects
#ifdef _WIN32

// reproc creates the child with CREATE_NEW_PROCESS_GROUP but no job object, so
// its TerminateProcess reaches only the direct child. that is not enough here:
// a cli-agent adapter launches the real agent binary as a GRANDCHILD, and a
// timeout would otherwise leave it running.
//
// the job is created with KILL_ON_JOB_CLOSE, so the whole tree is cleaned up
// even on the paths that return early. this is a deliberate divergence from
// subprocess.run, which only ever waits for the direct child.
struct job_handle
{
	HANDLE handle = nullptr;

	~job_handle()
	{
		if (handle != nullptr)
			CloseHandle(handle);
	}

	job_handle() = default;
	job_handle(const job_handle &) = delete;
	job_handle &operator=(const job_handle &) = delete;
};

static void job_adopt(job_handle &job, int pid)
{
	job.handle = CreateJobObjectW(nullptr, nullptr);
	if (job.handle == nullptr)
		return;

	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job.handle, JobObjectExtendedLimitInformation, &limits,
								 sizeof(limits)))
	{
		CloseHandle(job.handle);
		job.handle = nullptr;
		return;
	}

	HANDLE child = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
							   static_cast<DWORD>(pid));
	if (child == nullptr)
	{
		CloseHandle(job.handle);
		job.handle = nullptr;
		return;
	}

	// assignment can fail when the child already belongs to a job that forbids
	// nesting. reproc's own terminate/kill remains as the fallback.
	const BOOL assigned = AssignProcessToJobObject(job.handle, child);
	CloseHandle(child);
	if (!assigned)
	{
		CloseHandle(job.handle);
		job.handle = nullptr;
	}
}

static void job_terminate(job_handle &job)
{
	if (job.handle != nullptr)
		TerminateJobObject(job.handle, 1);
}

static void job_kill(job_handle &job) { job_terminate(job); }

#else

struct job_handle
{
	pid_t process_group = -1;
};

static void job_adopt(job_handle &job, int pid)
{
	// The child creates this group before exec; descendants inherit it.
	job.process_group = static_cast<pid_t>(pid);
}

static void job_terminate(job_handle &job)
{
	if (job.process_group > 0)
		kill(-job.process_group, SIGTERM);
}

static void job_kill(job_handle &job)
{
	if (job.process_group > 0)
		kill(-job.process_group, SIGKILL);
}

#endif

// ----------------------------------------------------------------- run

static void subprocess_halt(reproc::process &child, job_handle &job)
{
	job_terminate(job);
	child.terminate();
	child.wait(reproc::milliseconds(200));
	job_kill(job);
	child.kill();
	child.wait(reproc::milliseconds(200));
}

subprocess_result subprocess_run(const subprocess_options &options, odin_error &out_error)
{
	subprocess_result result;

	if (options.command.empty())
	{
		fail(out_error, error_kind::io, "no command was given");
		return result;
	}

	const std::string working_directory = file_path_utf8(options.working_directory);

	subprocess_ignore_sigpipe();

	reproc::options started;
	if (!working_directory.empty())
		started.working_directory = working_directory.c_str();
	std::map<std::string, std::string> environment;
	if (!subprocess_environment_build(options.inherit_environment, options.environment, environment,
									  out_error))
		return result;
	started.env.behavior = reproc::env::empty;
	started.env.extra = environment;
	started.redirect.in.type = reproc::redirect::pipe;
	started.redirect.out.type = reproc::redirect::pipe;
	started.redirect.err.type =
	  options.merge_stderr ? reproc::redirect::stdout_ : reproc::redirect::pipe;

	reproc::process child;
	std::error_code start_code;
#ifdef _WIN32
	start_code = child.start(options.command, started);
#else
	std::vector<char *> raw;
	raw.reserve(options.command.size() + 1);
	for (const std::string &part : options.command)
		raw.push_back(const_cast<char *>(part.c_str()));
	raw.push_back(nullptr);
	const auto [in_child, fork_code] = child.fork(started);
	start_code = fork_code;
	if (!start_code && in_child)
	{
		if (setpgid(0, 0) != 0)
			_exit(127);
		execvp(raw[0], raw.data());
		_exit(127);
	}
#endif
	if (start_code)
	{
		fail(out_error, error_kind::io, start_code.message());
		return result;
	}

	job_handle job;
	const auto [pid, pid_code] = child.pid();
	if (!pid_code)
		job_adopt(job, pid);

	const auto deadline = std::chrono::steady_clock::now() +
						  std::chrono::seconds(options.timeout_seconds);

	std::string raw_stdout;
	std::string raw_stderr;
	std::size_t written = 0;

	bool in_done = options.input.empty();
	bool out_done = false;
	bool err_done = options.merge_stderr;

	if (in_done)
		child.close(reproc::stream::in);

	while (!in_done || !out_done || !err_done)
	{
		reproc::milliseconds remaining = reproc::infinite;
		if (options.timeout_seconds > 0)
		{
			const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
			  deadline - std::chrono::steady_clock::now());
			if (left.count() <= 0)
			{
				subprocess_halt(child, job);
				fail(out_error, error_kind::io,
					 "Command '" + command_repr(options.command) + "' timed out after " +
					   std::to_string(options.timeout_seconds) + " seconds");
				return result;
			}
			remaining = reproc::milliseconds(static_cast<int>(left.count()));
		}

		int interests = 0;
		if (!in_done)
			interests |= reproc::event::in;
		if (!out_done)
			interests |= reproc::event::out;
		if (!err_done)
			interests |= reproc::event::err;

		const auto [events, poll_code] = child.poll(interests, remaining);
		if (poll_code)
			break;
		if (events == 0)
			continue; // timeout is re-evaluated at the top

		if ((events & reproc::event::in) != 0 && !in_done)
		{
			const auto [count, write_code] = child.write(
			  reinterpret_cast<const std::uint8_t *>(options.input.data()) + written,
			  options.input.size() - written);
			// a child that exits without reading closes the pipe; that is its
			// prerogative, not an error.
			if (write_code || count == 0)
			{
				in_done = true;
				child.close(reproc::stream::in);
			}
			else
			{
				written += count;
				if (written >= options.input.size())
				{
					in_done = true;
					child.close(reproc::stream::in);
				}
			}
		}

		if ((events & reproc::event::out) != 0 && !out_done)
		{
			std::uint8_t chunk[read_chunk];
			const auto [count, read_code] = child.read(reproc::stream::out, chunk, sizeof(chunk));
			if (read_code || count == 0)
				out_done = true;
			else
				raw_stdout.append(reinterpret_cast<const char *>(chunk), count);
		}

		if ((events & reproc::event::err) != 0 && !err_done)
		{
			std::uint8_t chunk[read_chunk];
			const auto [count, read_code] = child.read(reproc::stream::err, chunk, sizeof(chunk));
			if (read_code || count == 0)
				err_done = true;
			else
				raw_stderr.append(reinterpret_cast<const char *>(chunk), count);
		}
	}

	reproc::milliseconds wait_for = reproc::infinite;
	if (options.timeout_seconds > 0)
	{
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
		  deadline - std::chrono::steady_clock::now());
		wait_for = reproc::milliseconds(left.count() > 0 ? static_cast<int>(left.count()) : 0);
	}

	const auto [status, wait_code] = child.wait(wait_for);
	if (wait_code)
	{
		subprocess_halt(child, job);
		fail(out_error, error_kind::io,
			 "Command '" + command_repr(options.command) + "' timed out after " +
			   std::to_string(options.timeout_seconds) + " seconds");
		return result;
	}
	if (status == 127)
	{
		fail(out_error, error_kind::io,
			 "could not run command '" + command_repr(options.command) + "'");
		return result;
	}

	result.exit_code = status;
	result.stdout_text = utf8_sanitize(raw_stdout);
	result.stderr_text = utf8_sanitize(raw_stderr);
	if (options.redact_stdout)
		environment_redact(result.stdout_text, options);
	if (options.redact_stderr)
		environment_redact(result.stderr_text, options);
	return result;
}
