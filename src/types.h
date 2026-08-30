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

// mirrors the exception hierarchy in harness/errors.py one-to-one, so error
// text can stay byte-identical to the python implementation during the port.
//   config   -> WorkflowError raised from load_config
//   workflow -> WorkflowError raised from the engine or definitions
//   adapter  -> AdapterError
//   contract -> ContractError
//   io       -> the ValueError raised by harness/io.py
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
// product surface and is asserted verbatim by the parity tests.
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
