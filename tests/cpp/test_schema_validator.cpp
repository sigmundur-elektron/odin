#include <doctest/doctest.h>

#include "json_io.h"
#include "schema_validator.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// Every keyword is covered independently. The point is not coverage for its own
// sake: this validator replaced a third-party library, and the failure that
// would matter is not a wrong error message but an assertion that quietly stops
// being applied. A per-keyword negative case is what makes that visible.

static std::string errors_for(const char *schema_text, const char *instance_text)
{
	const json schema = json::parse(schema_text);
	const json instance = json::parse(instance_text);
	std::vector<schema_error> errors;
	schema_validate(schema, instance, errors);
	return schema_error_text(errors);
}

static bool accepts(const char *schema_text, const char *instance_text)
{
	return errors_for(schema_text, instance_text).empty();
}

// ------------------------------------------------------------------- type

TEST_CASE("type")
{
	CHECK(accepts(R"({"type":"string"})", R"("text")"));
	CHECK(accepts(R"({"type":"object"})", R"({})"));
	CHECK(accepts(R"({"type":"array"})", R"([])"));
	CHECK(accepts(R"({"type":"boolean"})", R"(true)"));
	CHECK(accepts(R"({"type":"null"})", R"(null)"));
	CHECK(accepts(R"({"type":"number"})", R"(1.5)"));
	CHECK(accepts(R"({"type":"integer"})", R"(3)"));

	CHECK(errors_for(R"({"type":"string"})", R"(3)") == "<root>: expected string, found integer");
	CHECK(errors_for(R"({"type":"object"})", R"([])") == "<root>: expected object, found array");
	CHECK(errors_for(R"({"type":"integer"})", R"("3")") == "<root>: expected integer, found string");

	SUBCASE("integer accepts a float with no fractional part, as 2020-12 requires")
	{
		CHECK(accepts(R"({"type":"integer"})", R"(3.0)"));
		CHECK_FALSE(accepts(R"({"type":"integer"})", R"(3.5)"));
	}

	SUBCASE("a type failure suppresses the keywords that no longer apply")
	{
		// otherwise a single wrong value produces two complaints, one of which
		// is noise
		CHECK(errors_for(R"({"type":"string","minLength":5})", R"(3)") ==
			  "<root>: expected string, found integer");
	}
}

// ------------------------------------------------------------ value checks

TEST_CASE("enum")
{
	CHECK(accepts(R"({"enum":["a","b"]})", R"("a")"));
	CHECK(errors_for(R"({"enum":["a","b"]})", R"("c")") ==
		  R"(<root>: "c" is not one of ["a", "b"])");
	// membership is by value, not by string form
	CHECK(accepts(R"({"enum":[1,{"k":[2]}]})", R"({"k":[2]})"));
}

TEST_CASE("const")
{
	CHECK(accepts(R"({"const":"bugfix"})", R"("bugfix")"));
	CHECK(errors_for(R"({"const":"bugfix"})", R"("feature")") == R"(<root>: must be "bugfix")");
}

// ----------------------------------------------------------------- strings

TEST_CASE("minLength")
{
	CHECK(accepts(R"({"minLength":1})", R"("a")"));
	CHECK(errors_for(R"({"minLength":1})", R"("")") == "<root>: must be at least 1 character");
	CHECK(errors_for(R"({"minLength":3})", R"("ab")") == "<root>: must be at least 3 characters");

	SUBCASE("counts characters, not bytes")
	{
		// three code points, six bytes. a byte-length check would accept this
		// against minLength 4 and reject it against 3.
		CHECK(accepts(R"({"minLength":3})", "\"\xc3\xa9\xc3\xa9\xc3\xa9\""));
		CHECK_FALSE(accepts(R"({"minLength":4})", "\"\xc3\xa9\xc3\xa9\xc3\xa9\""));
	}
}

TEST_CASE("pattern")
{
	CHECK(accepts(R"({"pattern":"^[a-z0-9][a-z0-9-]*$"})", R"("add-export")"));
	CHECK(errors_for(R"({"pattern":"^[a-z0-9][a-z0-9-]*$"})", R"("Add Export")") ==
		  R"(<root>: "Add Export" does not match ^[a-z0-9][a-z0-9-]*$)");

	SUBCASE("the bounded form used by task ids")
	{
		CHECK(accepts(R"({"pattern":"^[a-z0-9][a-z0-9-]{1,63}$"})", R"("ab")"));
		CHECK_FALSE(accepts(R"({"pattern":"^[a-z0-9][a-z0-9-]{1,63}$"})", R"("a")"));
	}

	SUBCASE("anchors are honoured rather than implied")
	{
		// an unanchored pattern matches a substring, which is what JSON Schema
		// specifies; using regex_match instead of regex_search would break it
		CHECK(accepts(R"({"pattern":"mid"})", R"("a-mid-z")"));
	}
}

// ----------------------------------------------------------------- numbers

TEST_CASE("minimum")
{
	CHECK(accepts(R"({"minimum":1})", R"(1)"));
	CHECK(accepts(R"({"minimum":1})", R"(2)"));
	CHECK(errors_for(R"({"minimum":1})", R"(0)") == "<root>: must be at least 1");
}

// ------------------------------------------------------------------ arrays

TEST_CASE("minItems")
{
	CHECK(accepts(R"({"minItems":1})", R"([1])"));
	CHECK(errors_for(R"({"minItems":1})", R"([])") == "<root>: must have at least 1 item");
	CHECK(errors_for(R"({"minItems":2})", R"([1])") == "<root>: must have at least 2 items");
}

TEST_CASE("maxItems")
{
	// present in agent.schema.json (rules, 8) and skill.schema.json
	// (procedure, 10). an earlier draft of the migration plan omitted it, which
	// would have made this validator reject Odin's own schemas as unsupported.
	CHECK(accepts(R"({"maxItems":2})", R"([1,2])"));
	CHECK(errors_for(R"({"maxItems":2})", R"([1,2,3])") == "<root>: must have at most 2 items");
	CHECK(errors_for(R"({"maxItems":1})", R"([1,2])") == "<root>: must have at most 1 item");
}

TEST_CASE("uniqueItems")
{
	CHECK(accepts(R"({"uniqueItems":true})", R"(["a","b"])"));
	CHECK(accepts(R"({"uniqueItems":false})", R"(["a","a"])"));
	CHECK(errors_for(R"({"uniqueItems":true})", R"(["a","a"])") ==
		  R"(<root>: must not contain duplicate items ("a" appears more than once))");
	// one complaint per array, not one per duplicated pair. `;` is the error
	// separator, so its absence here also proves no message embeds one.
	CHECK(errors_for(R"({"uniqueItems":true})", R"(["a","a","a"])").find(';') ==
		  std::string::npos);
}

TEST_CASE("items")
{
	CHECK(accepts(R"({"items":{"type":"string"}})", R"(["a","b"])"));
	CHECK(errors_for(R"({"items":{"type":"string"}})", R"(["a",3])") ==
		  "1: expected string, found integer");
	// applies to every element, and the index is part of the location
	CHECK(errors_for(R"({"items":{"type":"string"}})", R"([1,2])") ==
		  "0: expected string, found integer; 1: expected string, found integer");
}

// ----------------------------------------------------------------- objects

TEST_CASE("required")
{
	CHECK(accepts(R"({"required":["a"]})", R"({"a":1})"));
	CHECK(errors_for(R"({"required":["a","b"]})", R"({})") ==
		  "<root>: required property 'a' is missing; <root>: required property 'b' is missing");
	// present-but-null still counts as present
	CHECK(accepts(R"({"required":["a"]})", R"({"a":null})"));
}

TEST_CASE("minProperties")
{
	CHECK(accepts(R"({"minProperties":1})", R"({"a":1})"));
	CHECK(errors_for(R"({"minProperties":1})", R"({})") ==
		  "<root>: must have at least 1 property");
	CHECK(errors_for(R"({"minProperties":2})", R"({"a":1})") ==
		  "<root>: must have at least 2 properties");
}

TEST_CASE("properties")
{
	CHECK(accepts(R"({"properties":{"a":{"type":"string"}}})", R"({"a":"x"})"));
	// a declared property that is absent is not an error; that is `required`
	CHECK(accepts(R"({"properties":{"a":{"type":"string"}}})", R"({})"));
	CHECK(errors_for(R"({"properties":{"a":{"type":"string"}}})", R"({"a":1})") ==
		  "a: expected string, found integer");
}

TEST_CASE("additionalProperties")
{
	CHECK(accepts(R"({"properties":{"a":{}},"additionalProperties":false})", R"({"a":1})"));
	CHECK(errors_for(R"({"properties":{"a":{}},"additionalProperties":false})",
					 R"({"a":1,"b":2})") == "<root>: unexpected property 'b'");
	CHECK(accepts(R"({"properties":{"a":{}},"additionalProperties":true})", R"({"a":1,"b":2})"));

	SUBCASE("as a schema, it constrains the undeclared properties")
	{
		CHECK(accepts(R"({"additionalProperties":{"type":"string"}})", R"({"b":"x"})"));
		CHECK(errors_for(R"({"additionalProperties":{"type":"string"}})", R"({"b":2})") ==
			  "b: expected string, found integer");
	}
}

// ------------------------------------------------------------- composition

TEST_CASE("allOf")
{
	CHECK(accepts(R"({"allOf":[{"type":"string"},{"minLength":1}]})", R"("a")"));
	CHECK(errors_for(R"({"allOf":[{"minLength":2},{"pattern":"^z"}]})", R"("a")") ==
		  "<root>: must be at least 2 characters; <root>: \"a\" does not match ^z");
}

TEST_CASE("if and then")
{
	// the exact construct task.schema.json uses to demand `reproduction` only
	// for a bugfix
	const char *schema = R"({
	  "allOf": [{
	    "if": {"properties": {"kind": {"const": "bugfix"}}, "required": ["kind"]},
	    "then": {"required": ["reproduction"]}
	  }]
	})";

	CHECK(accepts(schema, R"({"kind":"feature"})"));
	CHECK(accepts(schema, R"({"kind":"bugfix","reproduction":{}})"));
	CHECK(errors_for(schema, R"({"kind":"bugfix"})") ==
		  "<root>: required property 'reproduction' is missing");

	SUBCASE("a failing `if` contributes no errors of its own")
	{
		// the condition's own violations must be discarded, or every feature
		// task would report that kind is not "bugfix"
		CHECK(accepts(schema, R"({"kind":"feature"})"));
	}

	SUBCASE("`required` inside `if` keeps the condition from matching an absent key")
	{
		CHECK(accepts(schema, R"({})"));
	}
}

// -------------------------------------------------------- supported check

TEST_CASE("schema_supported rejects anything outside the closed keyword set")
{
	std::string message;

	CHECK(schema_supported(json::parse(R"({"type":"string"})"), message));

	SUBCASE("an unknown assertion")
	{
		CHECK_FALSE(schema_supported(json::parse(R"({"format":"uuid"})"), message));
		CHECK(message == "unsupported keyword 'format' at <root>");
	}

	SUBCASE("an unknown assertion nested in a property")
	{
		CHECK_FALSE(schema_supported(
		  json::parse(R"({"properties":{"a":{"multipleOf":2}}})"), message));
		CHECK(message == "unsupported keyword 'multipleOf' at <root>/properties/a");
	}

	SUBCASE("composition keywords that would silently weaken validation")
	{
		for (const char *text : {R"({"oneOf":[]})", R"({"anyOf":[]})", R"({"not":{}})",
								 R"({"$ref":"#/x"})", R"({"$defs":{}})",
								 R"({"patternProperties":{}})"})
		{
			CAPTURE(text);
			CHECK_FALSE(schema_supported(json::parse(text), message));
		}
	}

	SUBCASE("annotations are accepted and assert nothing")
	{
		CHECK(schema_supported(
		  json::parse(R"({"$schema":"https://json-schema.org/draft/2020-12/schema",
						  "title":"t","description":"d"})"),
		  message));
	}

	SUBCASE("the legacy tuple form of items")
	{
		// silently ignoring it would validate no elements at all
		CHECK_FALSE(schema_supported(json::parse(R"({"items":[{"type":"string"}]})"), message));
	}

	SUBCASE("an uncompilable pattern is caught when the schema loads")
	{
		CHECK_FALSE(schema_supported(json::parse(R"({"pattern":"[unterminated"})"), message));
		CHECK(message.find("not a valid regular expression") != std::string::npos);
	}

	SUBCASE("an unknown type name")
	{
		CHECK_FALSE(schema_supported(json::parse(R"({"type":"int"})"), message));
	}
}

TEST_CASE("every bundled schema is supported in full")
{
	const fs::path root = fs::path(ODIN_REPO_ROOT) / "harness" / "schemas";
	for (const char *name : {"agent", "handoff", "skill", "task", "workflow"})
	{
		CAPTURE(name);
		odin_error err;
		const json schema = json_read(root / (std::string(name) + ".schema.json"), err);
		REQUIRE_FALSE(failed(err));

		std::string message;
		CHECK_MESSAGE(schema_supported(schema, message), message);
	}
}
