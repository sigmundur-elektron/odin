#pragma once
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

// the whole harness speaks one json dom. aliased so the dependency is stated
// once and the intent ("this is odin's wire/disk format") stays visible.
using json = nlohmann::json;

// odin's identifiers are strings on disk, not integers. aliased anyway so a
// signature says what it wants rather than just "another string".
using run_id = std::string;
using stage_id = std::string;
using agent_id = std::string;
using profile_id = std::string;

// a run is abandoned after this many transitions even when no single stage has
// exceeded its own attempt limit. overridden by [harness] in odin.toml.
constexpr int default_max_total_transitions = 40;

// applies to gates and adapters alike when the toml omits it
constexpr int default_timeout_seconds = 300;

// what went wrong, for callers that need to react differently rather than just
// report. the CLI maps every kind to a nonzero exit; the engine distinguishes an
// adapter failure (which becomes a blocked handoff) from a contract failure
// (which stops the run).
enum class error_kind : std::uint8_t
{
	none = 0,
	config,
	workflow,
	adapter,
	contract,
	io,
	count
};

// returned through a trailing out_error parameter rather than thrown, matching
// the out_actions convention already used elsewhere. `message` is user-facing
// product surface and is asserted verbatim by the tests.
struct odin_error
{
	error_kind kind = error_kind::none;
	std::string message;
};

inline bool failed(const odin_error &e) { return e.kind != error_kind::none; }

inline void fail(odin_error &out_error, error_kind kind, std::string message)
{
	out_error.kind = kind;
	out_error.message = std::move(message);
}
