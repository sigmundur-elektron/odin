#include "contracts.h"

#include <vector>

#include "atomic_file.h"
#include "json_io.h"
#include "schema_validator.h"

namespace fs = std::filesystem;

void contracts_configure(contracts &c, const fs::path &schema_root)
{
	c.schema_root = schema_root;
	c.cache.clear();
}

// returns nullptr with out_error set. the schema is checked for unsupported
// keywords exactly once, on the way into the cache.
static const json *contracts_schema(contracts &c, const std::string &contract,
									odin_error &out_error)
{
	const auto cached = c.cache.find(contract);
	if (cached != c.cache.end())
		return &cached->second;

	if (c.schema_root.empty())
	{
		fail(out_error, error_kind::contract, "contracts were used before being configured");
		return nullptr;
	}

	const fs::path path = c.schema_root / (contract + ".schema.json");
	if (!fs::exists(path))
	{
		fail(out_error, error_kind::contract, "unknown contract: " + contract);
		return nullptr;
	}

	odin_error read_error;
	json schema = json_read(path, read_error);
	if (failed(read_error))
	{
		fail(out_error, error_kind::contract, read_error.message);
		return nullptr;
	}

	// an unrecognised keyword is a hard failure rather than something skipped.
	// a validator that quietly ignores half a schema still returns "valid",
	// which is the one outcome worse than having no validator.
	std::string unsupported;
	if (!schema_supported(schema, unsupported))
	{
		fail(out_error, error_kind::contract,
			 file_path_utf8(path) + ": " + unsupported);
		return nullptr;
	}

	return &c.cache.emplace(contract, std::move(schema)).first->second;
}

void contract_validate(contracts &c,
					   const json &value,
					   const std::string &contract,
					   const std::string &where,
					   odin_error &out_error)
{
	const json *schema = contracts_schema(c, contract, out_error);
	if (schema == nullptr)
		return;

	std::vector<schema_error> errors;
	if (schema_validate(*schema, value, errors))
		return;

	fail(out_error, error_kind::contract,
		 where + " violates " + contract + ": " + schema_error_text(errors));
}
