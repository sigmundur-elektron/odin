#include "atomic_file.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

// a deliberately narrow alphabet, so a stray
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
	// the temporary file is created alongside its destination.
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

file_publish_result file_write_create_only(const fs::path &path,
										   const std::string &contents,
										   odin_error &out_error)
{
	const fs::path directory = path.parent_path();
	std::error_code directory_error;
	fs::create_directories(directory, directory_error);
	if (directory_error && !fs::is_directory(directory))
	{
		fail(out_error, error_kind::io,
			 "could not create " + file_path_utf8(directory) + " (" + directory_error.message() + ")");
		return file_publish_result::failed;
	}

	fs::path temporary;
#ifdef _WIN32
	HANDLE file = INVALID_HANDLE_VALUE;
	for (int attempt = 0; attempt < 20 && file == INVALID_HANDLE_VALUE; ++attempt)
	{
		temporary = directory / ("." + path.filename().string() + "." + temp_name_suffix());
		file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
						   FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS)
			break;
	}
	if (file == INVALID_HANDLE_VALUE)
	{
		fail(out_error, error_kind::io,
			 "could not create a temporary beside " + file_path_utf8(path));
		return file_publish_result::failed;
	}

	std::size_t written = 0;
	bool write_ok = true;
	while (written < contents.size())
	{
		DWORD count = 0;
		const DWORD wanted = static_cast<DWORD>(
		  std::min<std::size_t>(contents.size() - written, static_cast<std::size_t>(0xffffffffu)));
		if (!WriteFile(file, contents.data() + written, wanted, &count, nullptr) || count == 0)
		{
			write_ok = false;
			break;
		}
		written += count;
	}
	if (write_ok)
		write_ok = FlushFileBuffers(file) != 0;
	CloseHandle(file);
	if (!write_ok)
	{
		DeleteFileW(temporary.c_str());
		fail(out_error, error_kind::io, "could not write " + file_path_utf8(path));
		return file_publish_result::failed;
	}

	if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
		return file_publish_result::created;
	const DWORD publish_error = GetLastError();
	DeleteFileW(temporary.c_str());
	if (publish_error == ERROR_ALREADY_EXISTS || publish_error == ERROR_FILE_EXISTS)
		return file_publish_result::already_exists;
	fail(out_error, error_kind::io,
		 "could not publish " + file_path_utf8(path) + " (windows error " +
		   std::to_string(publish_error) + ")");
	return file_publish_result::failed;
#else
	int file = -1;
	for (int attempt = 0; attempt < 20 && file < 0; ++attempt)
	{
		temporary = directory / ("." + path.filename().string() + "." + temp_name_suffix());
		file = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (file < 0 && errno != EEXIST)
			break;
	}
	if (file < 0)
	{
		fail(out_error, error_kind::io,
			 "could not create a temporary beside " + file_path_utf8(path) + " (" +
			   std::strerror(errno) + ")");
		return file_publish_result::failed;
	}

	std::size_t written = 0;
	bool write_ok = true;
	while (written < contents.size())
	{
		const ssize_t count = write(file, contents.data() + written, contents.size() - written);
		if (count <= 0)
		{
			write_ok = false;
			break;
		}
		written += static_cast<std::size_t>(count);
	}
	if (write_ok)
		write_ok = fsync(file) == 0;
	close(file);
	if (!write_ok)
	{
		unlink(temporary.c_str());
		fail(out_error, error_kind::io, "could not write " + file_path_utf8(path));
		return file_publish_result::failed;
	}

	if (link(temporary.c_str(), path.c_str()) == 0)
	{
		unlink(temporary.c_str());
		const int directory_fd = open(directory.c_str(), O_RDONLY | O_CLOEXEC);
		if (directory_fd >= 0)
		{
			fsync(directory_fd);
			close(directory_fd);
		}
		return file_publish_result::created;
	}
	const int publish_error = errno;
	unlink(temporary.c_str());
	if (publish_error == EEXIST)
		return file_publish_result::already_exists;
	fail(out_error, error_kind::io,
		 "could not publish " + file_path_utf8(path) + " (" + std::strerror(publish_error) + ")");
	return file_publish_result::failed;
#endif
}
