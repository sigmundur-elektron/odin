#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

#include "types.h"

struct run_lock
{
	std::intptr_t native = -1;
	bool held = false;

	run_lock() = default;
	~run_lock();
	run_lock(const run_lock &) = delete;
	run_lock &operator=(const run_lock &) = delete;
};

bool run_lock_acquire(const std::filesystem::path &run_dir,
					  run_lock &out_lock,
					  odin_error &out_error);

std::filesystem::path run_journal_path(const std::filesystem::path &run_dir,
									   const json &record);

bool run_journal_publish(const std::filesystem::path &run_dir,
						 const json &record,
						 odin_error &out_error);

bool run_journal_load(const std::filesystem::path &run_dir,
					  std::vector<json> &out_records,
					  odin_error &out_error);
