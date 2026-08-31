#pragma once
#include <filesystem>
#include <string>

#include "types.h"

// render a path as utf-8 for error messages. on windows path::string() converts
// through the active code page and mangles non-ascii, so it is never used here.
std::string file_path_utf8(const std::filesystem::path &path);

// read a whole file as raw bytes. no encoding or newline translation - callers
// that need it do it explicitly, so the on-disk bytes are always visible.
std::string file_read_all(const std::filesystem::path &path, odin_error &out_error);

enum class file_publish_result
{
	created,
	already_exists,
	failed
};

// write `contents` to `path` so a concurrent reader sees either the complete old
// file or the complete new one, NEVER a partial write. the gui polls these files
// while a run is in flight, so this is an integration contract rather than an
// optimisation. parent directories are created as needed.
//
// the temporary is created in the target's own directory to keep the rename on a
// single volume, and is removed if any step fails.
void file_write_atomic(const std::filesystem::path &path,
					   const std::string &contents,
					   odin_error &out_error);

// Atomically publish a whole file without replacing an existing target. This is
// used for journal records whose immutability is part of the recovery protocol.
file_publish_result file_write_create_only(const std::filesystem::path &path,
										   const std::string &contents,
										   odin_error &out_error);
