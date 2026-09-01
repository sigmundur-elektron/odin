#include "schema_validator.h"

#include <cmath>
#include <regex>
#include <set>
#include <string>

namespace
{

const std::set<std::string> &ignored_keywords()
{
	// annotations. they carry no assertion, so accepting them is not a hole -
	// but they still have to be listed, or schema_supported would reject every
	// schema Odin ships.
	static const std::set<std::string> keywords = {"$schema", "title", "description"};
	return keywords;
}

const std::set<std::string> &assertion_keywords()
{
	static const std::set<std::string> keywords = {
	  "type", "required", "properties", "additionalProperties", "enum", "const",
	  "pattern", "minLength", "items", "minItems", "maxItems", "uniqueItems",
	  "minimum", "minProperties", "allOf", "if", "then"};
	return keywords;
}

std::string join_path(const std::string &parent, const std::string &part)
{
	return parent.empty() ? part : parent + "/" + part;
}

// the name JSON Schema uses for an instance's type, for "expected X, found Y".
std::string type_name(const json &value)
{
	if (value.is_object())
		return "object";
	if (value.is_array())
		return "array";
	if (value.is_string())
		return "string";
	if (value.is_boolean())
		return "boolean";
	if (value.is_null())
		return "null";
	if (value.is_number_integer())
		return "integer";
	if (value.is_number_float())
		return "number";
	return "unknown";
}

bool type_matches(const json &value, const std::string &wanted)
{
	if (wanted == "object")
		return value.is_object();
	if (wanted == "array")
		return value.is_array();
	if (wanted == "string")
		return value.is_string();
	if (wanted == "boolean")
		return value.is_boolean();
	if (wanted == "null")
		return value.is_null();
	if (wanted == "number")
		return value.is_number();
	if (wanted == "integer")
	{
		if (value.is_number_integer())
			return true;
		// 2020-12 treats a float with a zero fractional part as an integer, so
		// 1.0 satisfies "integer". a bare is_number_integer() check would
		// reject it and diverge from every other validator.
		if (value.is_number_float())
		{
			const double number = value.get<double>();
			return std::isfinite(number) && number == std::floor(number);
		}
		return false;
	}
	return false;
}

// minLength counts characters, not bytes. every leading utf-8 byte is one
// character; continuation bytes are 10xxxxxx and are skipped.
std::size_t utf8_length(const std::string &text)
{
	std::size_t count = 0;
	for (const unsigned char byte : text)
	{
		if ((byte & 0xC0) != 0x80)
			++count;
	}
	return count;
}

std::string plural(std::size_t count, const char *singular, const char *many)
{
	return std::to_string(count) + " " + (count == 1 ? singular : many);
}

std::string render_list(const json &values)
{
	std::string text = "[";
	bool first = true;
	for (const json &value : values)
	{
		if (!first)
			text += ", ";
		text += value.dump();
		first = false;
	}
	return text + "]";
}

void add(std::vector<schema_error> &out, const std::string &location, std::string message)
{
	out.push_back(schema_error{location, std::move(message)});
}

void validate_into(const json &schema, const json &instance, const std::string &location,
				   std::vector<schema_error> &out);

bool validates(const json &schema, const json &instance)
{
	std::vector<schema_error> discarded;
	validate_into(schema, instance, "", discarded);
	return discarded.empty();
}

void validate_into(const json &schema, const json &instance, const std::string &location,
				   std::vector<schema_error> &out)
{
	if (!schema.is_object())
		return;

	// --- type ---------------------------------------------------------
	//
	// checked first because every other keyword below is scoped to an
	// instance type, so a type mismatch would otherwise produce a second,
	// redundant complaint from whichever keyword happened to apply.
	const auto type = schema.find("type");
	if (type != schema.end() && type->is_string())
	{
		const std::string wanted = type->get<std::string>();
		if (!type_matches(instance, wanted))
		{
			add(out, location, "expected " + wanted + ", found " + type_name(instance));
			return;
		}
	}

	// --- value assertions, any type -----------------------------------
	const auto constant = schema.find("const");
	if (constant != schema.end() && instance != *constant)
		add(out, location, "must be " + constant->dump());

	const auto enumeration = schema.find("enum");
	if (enumeration != schema.end() && enumeration->is_array())
	{
		bool found = false;
		for (const json &candidate : *enumeration)
		{
			if (instance == candidate)
			{
				found = true;
				break;
			}
		}
		if (!found)
			add(out, location, instance.dump() + " is not one of " + render_list(*enumeration));
	}

	// --- strings ------------------------------------------------------
	if (instance.is_string())
	{
		const std::string text = instance.get<std::string>();

		const auto min_length = schema.find("minLength");
		if (min_length != schema.end() && min_length->is_number_unsigned())
		{
			const auto wanted = min_length->get<std::size_t>();
			if (utf8_length(text) < wanted)
				add(out, location, "must be at least " + plural(wanted, "character", "characters"));
		}

		const auto pattern = schema.find("pattern");
		if (pattern != schema.end() && pattern->is_string())
		{
			const std::string expression = pattern->get<std::string>();
			// schema_supported already compiled this once, so a throw here
			// would mean the schema changed underneath us.
			const std::regex compiled(expression, std::regex::ECMAScript);
			// search, not match: JSON Schema patterns are unanchored, and both
			// of Odin's carry their own ^ and $.
			if (!std::regex_search(text, compiled))
				add(out, location, instance.dump() + " does not match " + expression);
		}
	}

	// --- numbers ------------------------------------------------------
	if (instance.is_number())
	{
		const auto minimum = schema.find("minimum");
		if (minimum != schema.end() && minimum->is_number() &&
			instance.get<double>() < minimum->get<double>())
			add(out, location, "must be at least " + minimum->dump());
	}

	// --- arrays -------------------------------------------------------
	if (instance.is_array())
	{
		const auto min_items = schema.find("minItems");
		if (min_items != schema.end() && min_items->is_number_unsigned())
		{
			const auto wanted = min_items->get<std::size_t>();
			if (instance.size() < wanted)
				add(out, location, "must have at least " + plural(wanted, "item", "items"));
		}

		const auto max_items = schema.find("maxItems");
		if (max_items != schema.end() && max_items->is_number_unsigned())
		{
			const auto wanted = max_items->get<std::size_t>();
			if (instance.size() > wanted)
				add(out, location, "must have at most " + plural(wanted, "item", "items"));
		}

		const auto unique = schema.find("uniqueItems");
		if (unique != schema.end() && unique->is_boolean() && unique->get<bool>())
		{
			for (std::size_t left = 0; left < instance.size(); ++left)
			{
				for (std::size_t right = left + 1; right < instance.size(); ++right)
				{
					if (instance[left] == instance[right])
					{
						// no semicolon in the text: schema_error_text joins
						// errors with "; ", so one inside a message would make
						// the joined line ambiguous to read.
						add(out, location,
							"must not contain duplicate items (" + instance[left].dump() +
							  " appears more than once)");
						left = instance.size();
						break;
					}
				}
			}
		}

		const auto items = schema.find("items");
		if (items != schema.end() && items->is_object())
		{
			for (std::size_t at = 0; at < instance.size(); ++at)
				validate_into(*items, instance[at], join_path(location, std::to_string(at)), out);
		}
	}

	// --- objects ------------------------------------------------------
	//
	// the object's own assertions are reported before recursing, so a parent's
	// errors always precede its children's without needing a sort afterwards.
	if (instance.is_object())
	{
		const auto required = schema.find("required");
		if (required != schema.end() && required->is_array())
		{
			for (const json &name : *required)
			{
				if (name.is_string() && !instance.contains(name.get<std::string>()))
					add(out, location,
						"required property '" + name.get<std::string>() + "' is missing");
			}
		}

		const auto min_properties = schema.find("minProperties");
		if (min_properties != schema.end() && min_properties->is_number_unsigned())
		{
			const auto wanted = min_properties->get<std::size_t>();
			if (instance.size() < wanted)
				add(out, location, "must have at least " + plural(wanted, "property", "properties"));
		}

		const auto properties = schema.find("properties");
		const auto additional = schema.find("additionalProperties");

		if (additional != schema.end())
		{
			for (const auto &[name, value] : instance.items())
			{
				const bool declared = properties != schema.end() && properties->is_object() &&
									  properties->contains(name);
				if (declared)
					continue;

				if (additional->is_boolean() && !additional->get<bool>())
					add(out, location, "unexpected property '" + name + "'");
				else if (additional->is_object())
					validate_into(*additional, value, join_path(location, name), out);
			}
		}

		if (properties != schema.end() && properties->is_object())
		{
			// nlohmann backs objects with std::map, so this is ascending key
			// order on both sides - the traversal is deterministic by
			// construction rather than by a later sort.
			for (const auto &[name, subschema] : properties->items())
			{
				const auto present = instance.find(name);
				if (present != instance.end())
					validate_into(subschema, *present, join_path(location, name), out);
			}
		}
	}

	// --- composition --------------------------------------------------
	const auto all_of = schema.find("allOf");
	if (all_of != schema.end() && all_of->is_array())
	{
		for (const json &subschema : *all_of)
			validate_into(subschema, instance, location, out);
	}

	// `if` is a condition, not an assertion: its own failures are discarded and
	// only decide whether `then` applies. a schema with no `then` is a no-op.
	const auto condition = schema.find("if");
	const auto consequence = schema.find("then");
	if (condition != schema.end() && consequence != schema.end() && validates(*condition, instance))
		validate_into(*consequence, instance, location, out);
}

// ------------------------------------------------------------ schema check

bool schema_supported_at(const json &schema, const std::string &where, std::string &out_message);

bool schema_supported_child(const json &schema, const std::string &where, std::string &out_message)
{
	if (!schema.is_object())
	{
		out_message = where + " must be an object";
		return false;
	}
	return schema_supported_at(schema, where, out_message);
}

bool schema_supported_at(const json &schema, const std::string &where, std::string &out_message)
{
	for (const auto &[keyword, value] : schema.items())
	{
		if (ignored_keywords().count(keyword) != 0)
			continue;
		if (assertion_keywords().count(keyword) == 0)
		{
			out_message = "unsupported keyword '" + keyword + "' at " + where;
			return false;
		}

		const std::string here = join_path(where, keyword);

		if (keyword == "pattern")
		{
			if (!value.is_string())
			{
				out_message = here + " must be a string";
				return false;
			}
			try
			{
				const std::regex compiled(value.get<std::string>(), std::regex::ECMAScript);
				(void)compiled;
			}
			catch (const std::regex_error &error)
			{
				out_message = here + " is not a valid regular expression: " + error.what();
				return false;
			}
		}
		else if (keyword == "properties")
		{
			if (!value.is_object())
			{
				out_message = here + " must be an object";
				return false;
			}
			for (const auto &[name, subschema] : value.items())
			{
				if (!schema_supported_child(subschema, join_path(here, name), out_message))
					return false;
			}
		}
		else if (keyword == "items")
		{
			// the tuple form (an array of schemas) is legacy and unused here.
			// rejecting it is the point: silently ignoring it would validate
			// nothing at all.
			if (!schema_supported_child(value, here, out_message))
				return false;
		}
		else if (keyword == "if" || keyword == "then")
		{
			if (!schema_supported_child(value, here, out_message))
				return false;
		}
		else if (keyword == "allOf")
		{
			if (!value.is_array())
			{
				out_message = here + " must be an array";
				return false;
			}
			for (std::size_t at = 0; at < value.size(); ++at)
			{
				if (!schema_supported_child(value[at], join_path(here, std::to_string(at)),
											out_message))
					return false;
			}
		}
		else if (keyword == "additionalProperties")
		{
			if (value.is_object())
			{
				if (!schema_supported_at(value, here, out_message))
					return false;
			}
			else if (!value.is_boolean())
			{
				out_message = here + " must be a boolean or an object";
				return false;
			}
		}
		else if (keyword == "type")
		{
			static const std::set<std::string> names = {"object", "array",   "string", "boolean",
													   "null",   "integer", "number"};
			if (!value.is_string() || names.count(value.get<std::string>()) == 0)
			{
				out_message = here + " must be one of the seven JSON Schema type names";
				return false;
			}
		}
	}
	return true;
}

} // namespace

bool schema_supported(const json &schema, std::string &out_message)
{
	if (!schema.is_object())
	{
		out_message = "schema must be an object";
		return false;
	}
	return schema_supported_at(schema, "<root>", out_message);
}

bool schema_validate(const json &schema, const json &instance,
					 std::vector<schema_error> &out_errors)
{
	validate_into(schema, instance, "", out_errors);
	return out_errors.empty();
}

std::string schema_error_text(const std::vector<schema_error> &errors)
{
	std::string text;
	for (const schema_error &error : errors)
	{
		if (!text.empty())
			text += "; ";
		text += (error.location.empty() ? "<root>" : error.location) + ": " + error.message;
	}
	return text;
}
