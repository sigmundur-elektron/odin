#include <doctest/doctest.h>

#include "atomic_file.h"
#include "json_io.h"
#include "test_support.h"

#include <initializer_list>
#include <string>

// state files use the platform separator.
static std::string joined(std::initializer_list<const char *> lines)
{
#ifdef _WIN32
	const std::string eol = "\r\n";
#else
	const std::string eol = "\n";
#endif
	std::string out;
	for (const char *line : lines)
	{
		out += line;
		out += eol;
	}
	return out;
}

TEST_CASE("json_serialize produces the documented state-file bytes")
{
	// this fixture is the output of:
	//   json.dump(value, stream, indent=2, sort_keys=True); stream.write("\n")
	// pins all four serialisation requirements at
	// once: sorted keys, 2-space indent, \uXXXX escaping, and CRLF.
	json value;
	value["b"] = 1;
	value["a"]["x"] = json::array({1, 2});
	value["u"] = "caf\u00e9 \u4f60\u597d";
	value["f"] = 1.0;
	value["i"] = 0;
	value["n"] = nullptr;

	const std::string expected = joined({
	  "{",
	  R"(  "a": {)",
	  R"(    "x": [)",
	  "      1,",
	  "      2",
	  "    ]",
	  "  },",
	  R"(  "b": 1,)",
	  R"(  "f": 1.0,)",
	  R"(  "i": 0,)",
	  R"(  "n": null,)",
	  R"(  "u": "caf\u00e9 \u4f60\u597d")",
	  "}",
	});

	CHECK(json_serialize(value) == expected);
}

TEST_CASE("json_serialize keeps integers and floats distinct")
{
	// odin.toml writes `parameter_billions = 0`, which must stay an integer.
	// collapsing every number to double would silently change the file.
	json value;
	value["whole"] = 0;
	value["real"] = 0.0;

	const std::string text = json_serialize(value);
	// sorted, so "real" comes first and carries the comma
	CHECK(text.find("\"real\": 0.0,") != std::string::npos);
	CHECK(text.find("\"whole\": 0") != std::string::npos);
	CHECK(text.find("\"whole\": 0.0") == std::string::npos);
}

TEST_CASE("json_serialize sorts keys regardless of insertion order")
{
	json value;
	value["zebra"] = 1;
	value["alpha"] = 2;
	value["middle"] = 3;

	const std::string text = json_serialize(value);
	CHECK(text.find("alpha") < text.find("middle"));
	CHECK(text.find("middle") < text.find("zebra"));
}

TEST_CASE("json_write_atomic round trips through json_read")
{
	const temp_dir dir;
	const auto path = dir.path / "state.json";

	json value;
	value["status"] = "running";
	value["transitions"] = 3;
	value["stage_attempts"]["review"] = 2;

	odin_error err;
	json_write_atomic(path, value, err);
	REQUIRE_FALSE(failed(err));

	const json loaded = json_read(path, err);
	REQUIRE_FALSE(failed(err));
	CHECK(loaded == value);
}

TEST_CASE("json_write_atomic creates missing parent directories")
{
	const temp_dir dir;
	const auto path = dir.path / "runs" / "abc" / "events" / "001-review.json";

	odin_error err;
	json_write_atomic(path, json{{"sequence", 1}}, err);
	REQUIRE_FALSE(failed(err));
	CHECK(std::filesystem::exists(path));
}

TEST_CASE("json_write_atomic replaces an existing file and leaves no temporary")
{
	const temp_dir dir;
	const auto path = dir.path / "state.json";

	odin_error err;
	json_write_atomic(path, json{{"status", "running"}}, err);
	REQUIRE_FALSE(failed(err));
	json_write_atomic(path, json{{"status", "complete"}}, err);
	REQUIRE_FALSE(failed(err));

	const json loaded = json_read(path, err);
	REQUIRE_FALSE(failed(err));
	CHECK(loaded["status"] == "complete");

	int entries = 0;
	for (const auto &item : std::filesystem::directory_iterator(dir.path))
	{
		(void)item;
		++entries;
	}
	CHECK(entries == 1);
}

TEST_CASE("json_read names the missing file")
{
	const temp_dir dir;
	const auto path = dir.path / "absent.json";

	odin_error err;
	json_read(path, err);
	REQUIRE(failed(err));
	CHECK(err.kind == error_kind::io);
	CHECK(err.message == "file not found: " + file_path_utf8(path));
}

TEST_CASE("json_read rejects a top level array")
{
	const temp_dir dir;
	const auto path = dir.path / "array.json";
	temp_write(path, "[1, 2, 3]");

	odin_error err;
	json_read(path, err);
	REQUIRE(failed(err));
	CHECK(err.message == "expected a JSON object in " + file_path_utf8(path));
}

TEST_CASE("json_read rejects malformed json")
{
	const temp_dir dir;
	const auto path = dir.path / "broken.json";
	temp_write(path, "{\"status\": ");

	odin_error err;
	json_read(path, err);
	REQUIRE(failed(err));
	CHECK(err.message.rfind("invalid JSON in ", 0) == 0);
}

TEST_CASE("json_read accepts the CRLF that json_write_atomic produces")
{
	const temp_dir dir;
	const auto path = dir.path / "crlf.json";
	temp_write(path, "{\r\n  \"a\": 1\r\n}\r\n");

	odin_error err;
	const json loaded = json_read(path, err);
	REQUIRE_FALSE(failed(err));
	CHECK(loaded["a"] == 1);
}
