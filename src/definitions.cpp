#include "definitions.h"

#include "atomic_file.h"
#include "contracts.h"
#include "json_io.h"

namespace fs = std::filesystem;

void definitions_configure(definitions &d, sidecar &service, const fs::path &package_root)
{
	d.service = &service;
	d.package_root = package_root;
	d.cache.clear();
}

static const json *definitions_load(definitions &d,
									const char *kind,
									const std::string &identifier,
									const char *contract,
									bool enforce_identifier,
									odin_error &out_error)
{

	const std::string key = std::string(kind) + "/" + identifier;
	const auto cached = d.cache.find(key);
	if (cached != d.cache.end())
		return &cached->second;

	if (d.service == nullptr)
	{
		fail(out_error, error_kind::workflow, "definitions were used before being configured");
		return nullptr;
	}

	const fs::path path = d.package_root / kind / (identifier + ".json");

	odin_error read_error;
	json value = json_read(path, read_error);
	if (failed(read_error))
	{
		// python re-raises the io ValueError as a WorkflowError, message intact
		fail(out_error, error_kind::workflow, read_error.message);
		return nullptr;
	}

	contract_validate(*d.service, value, contract, file_path_utf8(path), out_error);
	if (failed(out_error))
		return nullptr;

	if (enforce_identifier)
	{
		// python interpolates value.get("id") directly, so a missing key renders
		// as None and a non-string renders as its literal.
		std::string actual = "None";
		const auto id = value.find("id");
		if (id != value.end())
			actual = id->is_string() ? id->get<std::string>() : id->dump();

		if (actual != identifier)
		{
			fail(out_error, error_kind::workflow,
				 file_path_utf8(path) + ": id '" + actual + "' does not match filename '" + identifier + "'");
			return nullptr;
		}
	}

	return &d.cache.emplace(key, std::move(value)).first->second;
}

const json *definitions_load_agent(definitions &d, const std::string &identifier, odin_error &out_error)
{
	return definitions_load(d, "agents", identifier, "agent", true, out_error);
}

const json *definitions_load_skill(definitions &d, const std::string &identifier, odin_error &out_error)
{
	return definitions_load(d, "skills", identifier, "skill", true, out_error);
}

const json *definitions_load_workflow(definitions &d, const std::string &identifier, odin_error &out_error)
{
	return definitions_load(d, "workflows", identifier, "workflow", true, out_error);
}

// a template is a task, and tasks carry their own id rather than matching a
// filename - "feature.json" holds whatever id the author chose.
const json *definitions_load_template(definitions &d, const std::string &identifier, odin_error &out_error)
{
	return definitions_load(d, "templates", identifier, "task", false, out_error);
}
