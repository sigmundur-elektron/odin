#pragma once
#include <string>
#include <vector>

#include "types.h"

// A JSON Schema validator covering exactly the keywords Odin's own schemas use.
//
// This replaces a Python `jsonschema` sidecar. It is deliberately not a general
// draft 2020-12 implementation: Odin owns every schema it validates against, so
// the supported set is closed and known. schema_supported() enforces that -
// an unrecognised keyword is an error rather than something quietly ignored,
// because a validator that silently under-validates is worse than no validator.
//
// Supported assertions:
//   type required properties additionalProperties enum const pattern minLength
//   items minItems maxItems uniqueItems minimum minProperties allOf if then
//
// Accepted and ignored, because they assert nothing:
//   $schema title description

struct schema_error
{
	// "" at the root, otherwise a slash-joined instance path such as
	// "stages/0/on". rendered as "<root>" by schema_error_text.
	std::string location;
	std::string message;
};

// Reject a schema that uses anything outside the supported set, or that is
// structurally malformed (a non-object subschema, a tuple-form `items`, an
// uncompilable `pattern`). out_message names the offending schema location.
//
// Call this once when a schema is loaded, not per validation.
bool schema_supported(const json &schema, std::string &out_message);

// Returns true when `instance` satisfies `schema`.
//
// Errors are appended in a defined order: within one subschema, object-level
// assertions are reported before the properties they contain, so a parent's
// errors always precede its children's. Sibling properties are visited in
// ascending key order. The order is a product of traversal rather than a sort,
// so it is stable without needing a comparator over mixed string/index paths -
// which is exactly the case that makes the Python implementation raise.
bool schema_validate(const json &schema, const json &instance,
					 std::vector<schema_error> &out_errors);

// "status: ...; summary: ..." - the tail of a contract failure message.
std::string schema_error_text(const std::vector<schema_error> &errors);
