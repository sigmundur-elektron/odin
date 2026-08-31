#pragma once
#include <filesystem>
#include <string>

#include "atomic_file.h"
#include "types.h"

// serialise exactly as harness/io.py does:
//   json.dump(value, stream, indent=2, sort_keys=True); stream.write("\n")
//
// three details are load-bearing and each is easy to get silently wrong:
//   * sorted keys      - nlohmann's default std::map backing already does this
//   * ensure_ascii     - python escapes non-ascii to \uXXXX by default
//   * native newlines  - python opens the stream in TEXT mode, so every \n
//                        becomes \r\n on windows. state files are CRLF there.
//
// exposed separately from the file write so parity tests can assert on bytes.
std::string json_serialize(const json &value);

// read a json object. matching harness/io.py, a top-level array or scalar is an
// error: every file odin owns is an object.
json json_read(const std::filesystem::path &path, odin_error &out_error);

// serialise, then write through file_write_atomic.
void json_write_atomic(const std::filesystem::path &path,
					   const json &value,
					   odin_error &out_error);

file_publish_result json_write_create_only(const std::filesystem::path &path,
										   const json &value,
										   odin_error &out_error);
