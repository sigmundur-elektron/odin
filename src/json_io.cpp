#include "json_io.h"

#include "atomic_file.h"

namespace fs = std::filesystem;

// python writes these files in text mode, so the newlines json.dump emits are
// translated to the platform separator. only structural newlines are affected:
// a newline inside a string is already escaped as the two characters \ and n by
// the time it reaches here, so this cannot corrupt content.
static std::string apply_native_newlines(const std::string &text)
{
#ifdef _WIN32
	std::string out;
	out.reserve(text.size() + text.size() / 16);
	for (const char c : text)
	{
		if (c == '\n')
			out.push_back('\r');
		out.push_back(c);
	}
	return out;
#else
	return text;
#endif
}

std::string json_serialize(const json &value)
{
	// indent 2, space fill, ensure_ascii, and u+fffd for undecodable input -
	// the last one mirrors errors="replace" on the python side.
	std::string text = value.dump(2, ' ', true, json::error_handler_t::replace);
	text.push_back('\n');
	return apply_native_newlines(text);
}

json json_read(const fs::path &path, odin_error &out_error)
{
	const std::string contents = file_read_all(path, out_error);
	if (failed(out_error))
		return json::object();

	// note: nlohmann's parse diagnostics do not match python's JSONDecodeError
	// text. that divergence is accepted - it only surfaces for a corrupt file,
	// never for state odin itself wrote.
	json value = json::parse(contents, nullptr, false);
	if (value.is_discarded())
	{
		fail(out_error, error_kind::io, "invalid JSON in " + file_path_utf8(path));
		return json::object();
	}
	if (!value.is_object())
	{
		fail(out_error, error_kind::io, "expected a JSON object in " + file_path_utf8(path));
		return json::object();
	}
	return value;
}

void json_write_atomic(const fs::path &path, const json &value, odin_error &out_error)
{
	file_write_atomic(path, json_serialize(value), out_error);
}

file_publish_result json_write_create_only(const fs::path &path,
										   const json &value,
										   odin_error &out_error)
{
	return file_write_create_only(path, json_serialize(value), out_error);
}
