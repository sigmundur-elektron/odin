#pragma once
#include <filesystem>
#include <string>

#include "atomic_file.h"
#include "types.h"

// serialise a state file: two-space indent, sorted keys, trailing newline.
//
// three details are load-bearing and each is easy to get silently wrong:
//   * sorted keys      - nlohmann's default std::map backing already does this
//   * ensure_ascii     - non-ascii is escaped to \uXXXX, so the bytes are safe
//                        for any terminal or editor that reads the file
//   * native newlines  - state files use the platform separator, so they are
//                        CRLF on windows and diff cleanly there.
//
// exposed separately from the file write so tests can assert on the bytes.
std::string json_serialize(const json &value);

// read a json object. a top-level array or scalar is an error: every file odin
// owns is an object.
json json_read(const std::filesystem::path &path, odin_error &out_error);

// serialise, then write through file_write_atomic.
void json_write_atomic(const std::filesystem::path &path,
					   const json &value,
					   odin_error &out_error);

file_publish_result json_write_create_only(const std::filesystem::path &path,
										   const json &value,
										   odin_error &out_error);
