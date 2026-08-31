#include <doctest/doctest.h>

#include "atomic_file.h"
#include "json_io.h"
#include "run_store.h"
#include "test_support.h"

TEST_CASE("create-only publication preserves the first complete value")
{
	const temp_dir dir;
	const auto path = dir.path / "journal" / "record.json";
	odin_error err;

	CHECK(json_write_create_only(path, json{{"value", 1}}, err) == file_publish_result::created);
	REQUIRE_FALSE(failed(err));
	CHECK(json_write_create_only(path, json{{"value", 2}}, err) ==
		  file_publish_result::already_exists);
	REQUIRE_FALSE(failed(err));
	CHECK(json_read(path, err).at("value") == 1);
}

TEST_CASE("a run lock excludes another runner and can be reacquired")
{
	const temp_dir dir;
	odin_error err;
	{
		run_lock first;
		REQUIRE(run_lock_acquire(dir.path, first, err));

		run_lock second;
		CHECK_FALSE(run_lock_acquire(dir.path, second, err));
		REQUIRE(failed(err));
		CHECK(err.message.find("already being executed") != std::string::npos);
	}
	err = {};
	run_lock second;
	CHECK(run_lock_acquire(dir.path, second, err));
	CHECK(std::filesystem::exists(dir.path / "run.lock"));
}

TEST_CASE("journal publication is idempotent but rejects conflicting content")
{
	const temp_dir dir;
	json record{{"journal_version", 1},
				{"type", "stage_started"},
				{"execution_id", "abc"},
				{"sequence", 1},
				{"stage", "specify"},
				{"kind", "agent"},
				{"attempt", 1},
				{"at", "now"}};
	odin_error err;
	REQUIRE(run_journal_publish(dir.path, record, err));
	CHECK(run_journal_publish(dir.path, record, err));

	record["stage"] = "other";
	CHECK_FALSE(run_journal_publish(dir.path, record, err));
	CHECK(err.message.find("immutable journal record conflicts") != std::string::npos);
}

TEST_CASE("journal loading rejects mismatched records for one execution")
{
	const temp_dir dir;
	json started{{"journal_version", 1},
				 {"type", "stage_started"},
				 {"execution_id", "same"},
				 {"sequence", 1},
				 {"stage", "specify"},
				 {"kind", "agent"},
				 {"attempt", 1},
				 {"at", "now"}};
	json completed = started;
	completed["type"] = "stage_completed";
	completed["stage"] = "other";
	completed["result"] = json::object();
	completed["metadata"] = json::object();

	odin_error err;
	REQUIRE(run_journal_publish(dir.path, started, err));
	REQUIRE(run_journal_publish(dir.path, completed, err));
	std::vector<json> journal;
	CHECK_FALSE(run_journal_load(dir.path, journal, err));
	CHECK_FALSE(err.message.empty());
}

TEST_CASE("journal loading rejects malformed field types without throwing")
{
	const temp_dir dir;
	const auto path = dir.path / "journal" / "000001-001-x-0-started.json";
	temp_write(path,
			   R"({"journal_version":"one","type":1,"execution_id":"x","sequence":-1,"attempt":0,"stage":"s","kind":"agent","at":"now"})");

	odin_error err;
	std::vector<json> journal;
	CHECK_FALSE(run_journal_load(dir.path, journal, err));
	CHECK(err.message.find("invalid journal record") != std::string::npos);
}

TEST_CASE("journal loading rejects a completed record without a handoff")
{
	const temp_dir dir;
	const json started{{"journal_version", 1},
					   {"type", "stage_started"},
					   {"execution_id", "missing-handoff"},
					   {"sequence", 1},
					   {"stage", "specify"},
					   {"kind", "agent"},
					   {"attempt", 1},
					   {"at", "now"}};
	json completed = started;
	completed["type"] = "stage_completed";
	completed["result"] = json::object();
	completed["metadata"] = json::object();
	odin_error err;
	REQUIRE(run_journal_publish(dir.path, started, err));
	REQUIRE(run_journal_publish(dir.path, completed, err));
	std::vector<json> journal;
	CHECK_FALSE(run_journal_load(dir.path, journal, err));
	CHECK(err.message.find("invalid completed handoff") != std::string::npos);
}

TEST_CASE("a journal path that is not a directory is an io error")
{
	const temp_dir dir;
	temp_write(dir.path / "journal", "not a directory");
	odin_error err;
	std::vector<json> journal;
	CHECK_FALSE(run_journal_load(dir.path, journal, err));
	CHECK(err.kind == error_kind::io);
}
