#include "atomic_file.h"

#include <chrono>
#include <fstream>
#include <random>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

// matches the alphabet python's tempfile.mkstemp draws from, so a stray
// temporary left by either implementation looks the same to a human.
static const char temp_name_alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_";
constexpr int temp_name_length = 8;

static std::string temp_name_suffix()
{
	static thread_local std::mt19937 engine{std::random_device{}()};
	std::uniform_int_distribution<int> pick(0, static_cast<int>(sizeof(temp_name_alphabet)) - 2);

	std::string suffix;
	suffix.reserve(temp_name_length);
	for (int i = 0; i < temp_name_length; ++i) suffix.push_back(temp_name_alphabet[pick(engine)]);
	return suffix;
}

std::string file_path_utf8(const fs::path &path)
{
	const std::u8string native = path.u8string();
	return std::string(native.begin(), native.end());
}

std::string file_read_all(const fs::path &path, odin_error &out_error)
{
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
	{
		fail(out_error, error_kind::io, "file not found: " + file_path_utf8(path));
		return {};
	}

	std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
	if (stream.bad())
	{
		fail(out_error, error_kind::io, "could not read " + file_path_utf8(path));
		return {};
	}
	return contents;
}

// replace `from` with `to` atomically.
//
// on windows this can legitimately fail while a reader holds the target open -
// exactly what a polling gui does - so a short retry is not paranoia. a reader
// that opens without FILE_SHARE_DELETE will still block us out entirely.
static bool file_replace(const fs::path &from, const fs::path &to, odin_error &out_error)
{
	constexpr int attempts = 5;
	constexpr auto backoff = std::chrono::milliseconds(10);

#ifdef _WIN32
	DWORD last = 0;
	for (int attempt = 0; attempt < attempts; ++attempt)
	{
		if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING))
			return true;

		last = GetLastError();
		const bool contended = last == ERROR_ACCESS_DENIED || last == ERROR_SHARING_VIOLATION || last == ERROR_LOCK_VIOLATION;
		if (!contended)
			break;

		std::this_thread::sleep_for(backoff);
	}
	fail(out_error, error_kind::io,
		 "could not replace " + file_path_utf8(to) + " (windows error " + std::to_string(last) + ")");
	return false;
#else
	std::error_code code;
	for (int attempt = 0; attempt < attempts; ++attempt)
	{
		fs::rename(from, to, code);
		if (!code)
			return true;
		std::this_thread::sleep_for(backoff);
	}
	fail(out_error, error_kind::io,
		 "could not replace " + file_path_utf8(to) + " (" + code.message() + ")");
	return false;
#endif
}

void file_write_atomic(const fs::path &path, const std::string &contents, odin_error &out_error)
{
	const fs::path directory = path.parent_path();

	if (!directory.empty())
	{
		std::error_code code;
		fs::create_directories(directory, code);
		if (code && !fs::is_directory(directory))
		{
			fail(out_error, error_kind::io,
				 "could not create " + file_path_utf8(directory) + " (" + code.message() + ")");
			return;
		}
	}

	// ".<name>.<random>" in the target's own directory, mirroring the prefix
	// python's write_json_atomic passes to mkstemp.
	const fs::path temporary =
	  directory / ("." + path.filename().string() + "." + temp_name_suffix());

	{
		std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
		if (!stream)
		{
			fail(out_error, error_kind::io,
				 "could not create a temporary beside " + file_path_utf8(path));
			return;
		}
		stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
		stream.flush();
		if (!stream)
		{
			stream.close();
			std::error_code ignored;
			fs::remove(temporary, ignored);
			fail(out_error, error_kind::io, "could not write " + file_path_utf8(path));
			return;
		}
	}

	if (!file_replace(temporary, path, out_error))
	{
		std::error_code ignored;
		fs::remove(temporary, ignored);
	}
}
