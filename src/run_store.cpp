#include "run_store.h"

#include <algorithm>
#include <cstdio>

#include "atomic_file.h"
#include "json_io.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

run_lock::~run_lock()
{
	if (!held)
		return;
#ifdef _WIN32
	HANDLE handle = reinterpret_cast<HANDLE>(native);
	OVERLAPPED range = {};
	UnlockFileEx(handle, 0, 1, 0, &range);
	CloseHandle(handle);
#else
	flock(static_cast<int>(native), LOCK_UN);
	close(static_cast<int>(native));
#endif
}

bool run_lock_acquire(const fs::path &run_dir, run_lock &out_lock, odin_error &out_error)
{
	const fs::path path = run_dir / "run.lock";
#ifdef _WIN32
	HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
								FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
								OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		fail(out_error, error_kind::io, "could not open run lock " + file_path_utf8(path));
		return false;
	}
	OVERLAPPED range = {};
	if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
					&range))
	{
		const DWORD code = GetLastError();
		CloseHandle(handle);
		if (code == ERROR_LOCK_VIOLATION)
			fail(out_error, error_kind::workflow,
				 "run '" + file_path_utf8(run_dir) + "' is already being executed");
		else
			fail(out_error, error_kind::io, "could not lock run " + file_path_utf8(run_dir));
		return false;
	}
	out_lock.native = reinterpret_cast<std::intptr_t>(handle);
#else
	const int descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (descriptor < 0)
	{
		fail(out_error, error_kind::io,
			 "could not open run lock " + file_path_utf8(path) + " (" + std::strerror(errno) + ")");
		return false;
	}
	if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
	{
		const int code = errno;
		close(descriptor);
		if (code == EWOULDBLOCK || code == EAGAIN)
			fail(out_error, error_kind::workflow,
				 "run '" + file_path_utf8(run_dir) + "' is already being executed");
		else
			fail(out_error, error_kind::io,
				 "could not lock run " + file_path_utf8(run_dir) + " (" + std::strerror(code) + ")");
		return false;
	}
	out_lock.native = descriptor;
#endif
	out_lock.held = true;
	return true;
}

fs::path run_journal_path(const fs::path &run_dir, const json &record)
{
	const std::string type = record.value("type", std::string{});
	const int phase = type == "stage_started" ? 0 : type == "stage_completed" ? 1
																			  : 2;
	const std::string suffix = type == "stage_started" ? "started" : type == "stage_completed" ? "completed"
																							   : "abandoned";
	char prefix[48];
	std::snprintf(prefix, sizeof(prefix), "%06d-%03d-", record.value("sequence", 0),
				  record.value("attempt", 0));
	return run_dir / "journal" /
		   (std::string(prefix) + record.value("execution_id", std::string{}) + "-" +
			std::to_string(phase) + "-" + suffix + ".json");
}

bool run_journal_publish(const fs::path &run_dir, const json &record, odin_error &out_error)
{
	const fs::path path = run_journal_path(run_dir, record);
	const file_publish_result published = json_write_create_only(path, record, out_error);
	if (published == file_publish_result::created)
		return true;
	if (published == file_publish_result::already_exists)
	{
		odin_error read_error;
		const json existing = json_read(path, read_error);
		if (!failed(read_error) && existing == record)
			return true;
		fail(out_error, error_kind::workflow,
			 "immutable journal record conflicts with " + file_path_utf8(path));
	}
	return false;
}

bool run_journal_load(const fs::path &run_dir,
					  std::vector<json> &out_records,
					  odin_error &out_error)
{
	out_records.clear();
	const fs::path directory = run_dir / "journal";
	std::error_code filesystem_error;
	if (!fs::exists(directory, filesystem_error))
		return true;
	if (filesystem_error || !fs::is_directory(directory, filesystem_error))
	{
		fail(out_error, error_kind::io,
			 "could not read journal directory " + file_path_utf8(directory));
		return false;
	}

	std::vector<fs::path> paths;
	fs::directory_iterator entries(directory, filesystem_error);
	const fs::directory_iterator end;
	while (!filesystem_error && entries != end)
	{
		const fs::directory_entry &entry = *entries;
		if (entry.is_regular_file() && entry.path().extension() == ".json")
			paths.push_back(entry.path());
		entries.increment(filesystem_error);
	}
	if (filesystem_error)
	{
		fail(out_error, error_kind::io,
			 "could not read journal directory " + file_path_utf8(directory) + " (" +
			   filesystem_error.message() + ")");
		return false;
	}
	std::sort(paths.begin(), paths.end());
	for (const fs::path &path : paths)
	{
		json record = json_read(path, out_error);
		if (failed(out_error))
			return false;
		const auto version = record.find("journal_version");
		const auto type_value = record.find("type");
		const std::string type = type_value != record.end() && type_value->is_string() ? type_value->get<std::string>() : std::string{};
		if (version == record.end() || !version->is_number_integer() || version->get<int>() != 1 ||
			(type != "stage_started" && type != "stage_completed") ||
			!record.contains("execution_id") || !record.at("execution_id").is_string() ||
			record.at("execution_id").get_ref<const std::string &>().empty() ||
			!record.contains("sequence") || !record.at("sequence").is_number_integer() ||
			record.at("sequence").get<int>() < 1 ||
			!record.contains("attempt") || !record.at("attempt").is_number_integer() ||
			record.at("attempt").get<int>() < 1 ||
			!record.contains("stage") || !record.at("stage").is_string() ||
			record.at("stage").get_ref<const std::string &>().empty() ||
			!record.contains("kind") || !record.at("kind").is_string() ||
			record.at("kind").get_ref<const std::string &>().empty() ||
			!record.contains("at") || !record.at("at").is_string() ||
			(type == "stage_completed" &&
			 (!record.contains("result") || !record.at("result").is_object() ||
			  !record.contains("metadata") || !record.at("metadata").is_object())))
		{
			fail(out_error, error_kind::workflow,
				 "invalid journal record " + file_path_utf8(path));
			return false;
		}
		if (type == "stage_completed")
		{
			const json &result = record.at("result");
			const auto status = result.find("status");
			const auto summary = result.find("summary");
			const auto artifacts = result.find("artifacts");
			const auto findings = result.find("findings");
			if (status == result.end() || !status->is_string() ||
				(status->get<std::string>() != "approved" && status->get<std::string>() != "revision" &&
				 status->get<std::string>() != "blocked") ||
				summary == result.end() || !summary->is_string() ||
				artifacts == result.end() || !artifacts->is_object() ||
				findings == result.end() || !findings->is_array())
			{
				fail(out_error, error_kind::workflow,
					 "invalid completed handoff in " + file_path_utf8(path));
				return false;
			}
		}
		if (path.filename() != run_journal_path(run_dir, record).filename())
		{
			fail(out_error, error_kind::workflow,
				 "journal filename does not match record identity: " + file_path_utf8(path));
			return false;
		}
		out_records.push_back(std::move(record));
	}

	for (std::size_t i = 0; i < out_records.size(); ++i)
	{
		const json &record = out_records[i];
		int same_type = 0;
		bool paired = record.at("type") == "stage_started";
		for (const json &candidate : out_records)
		{
			if (record.at("execution_id") != candidate.at("execution_id"))
				continue;
			if (record.at("type") == candidate.at("type"))
				++same_type;
			else
				paired = true;
		}
		if (same_type != 1 || !paired)
		{
			fail(out_error, error_kind::workflow,
				 "journal pairing conflict for execution '" +
				   record.at("execution_id").get<std::string>() + "'");
			return false;
		}
		for (std::size_t j = i + 1; j < out_records.size(); ++j)
		{
			const json &other = out_records[j];
			if (record.at("execution_id") != other.at("execution_id"))
				continue;
			if (record.at("type") == other.at("type") ||
				record.at("sequence") != other.at("sequence") ||
				record.at("attempt") != other.at("attempt") ||
				record.at("stage") != other.at("stage") || record.at("kind") != other.at("kind"))
			{
				fail(out_error, error_kind::workflow,
					 "journal identity conflict for execution '" +
					   record.at("execution_id").get<std::string>() + "'");
				return false;
			}
		}
	}

	for (std::size_t i = 0; i < out_records.size(); ++i)
	{
		const json &record = out_records[i];
		for (std::size_t j = i + 1; j < out_records.size(); ++j)
		{
			const json &other = out_records[j];
			if (record.at("type") == "stage_started" && other.at("type") == "stage_started" &&
				record.at("sequence") == other.at("sequence") &&
				record.at("attempt") == other.at("attempt"))
			{
				fail(out_error, error_kind::workflow,
					 "journal has duplicate attempt identity at sequence " +
					   std::to_string(record.at("sequence").get<int>()));
				return false;
			}
			if (record.at("type") == "stage_completed" && other.at("type") == "stage_completed" &&
				record.at("sequence") == other.at("sequence"))
			{
				fail(out_error, error_kind::workflow,
					 "journal has multiple completions at sequence " +
					   std::to_string(record.at("sequence").get<int>()));
				return false;
			}
		}
	}
	return true;
}
