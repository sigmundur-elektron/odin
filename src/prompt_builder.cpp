#include "prompt_builder.h"

#include <string>

const char *const prompt_handoff_shape =
  "{\"status\": \"approved\" | \"revision\" | \"blocked\", "
  "\"summary\": \"one sentence\", "
  "\"artifacts\": {}, "
  "\"findings\": [\"blocking issue\", \"...\"]}";

namespace
{

std::string string_field(const json &value, const char *key, const char *fallback)
{
	if (!value.is_object())
		return fallback;
	const auto found = value.find(key);
	return found != value.end() && found->is_string() ? found->get<std::string>() : fallback;
}

// truncate to at most `characters` code points, never mid-sequence
std::string truncate_characters(const std::string &text, std::size_t characters, bool &out_cut)
{
	std::size_t at = 0;
	std::size_t seen = 0;
	while (at < text.size() && seen < characters)
	{
		++at;
		while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
			++at;
		++seen;
	}
	out_cut = at < text.size();
	return text.substr(0, at);
}

// compact, as in json.dumps(separators=(",",":"))
std::string compact(const json &value)
{
	return value.dump(-1, ' ', false, json::error_handler_t::replace);
}

// indented and key-sorted. nlohmann sorts object keys by construction.
std::string pretty(const json &value)
{
	return value.dump(2, ' ', false, json::error_handler_t::replace);
}

const json &field_or(const json &value, const char *key, const json &fallback)
{
	if (!value.is_object())
		return fallback;
	const auto found = value.find(key);
	return found == value.end() ? fallback : *found;
}

} // namespace

prompt_messages prompt_build(const json &request, int max_context_chars)
{
	static const json empty_object = json::object();
	static const json empty_array = json::array();

	const json &agent = field_or(request, "agent", empty_object);
	const json &skills = field_or(request, "skills", empty_array);
	const json &stage = field_or(request, "stage", empty_object);
	const json &task = field_or(request, "task", empty_object);
	const json &artifacts = field_or(request, "artifacts", empty_object);

	std::string system;
	system += "You are the '" + string_field(agent, "id", "agent") +
			  "' stage of an automated software workflow.\n";
	system += "Purpose: " + string_field(agent, "purpose", "") + "\n";

	const json &rules = field_or(agent, "rules", empty_array);
	if (rules.is_array() && !rules.empty())
	{
		system += "Rules:\n";
		for (const json &rule : rules)
		{
			if (rule.is_string())
				system += "- " + rule.get<std::string>() + "\n";
		}
	}

	// skills are rendered compact: they are reference data, and a model given
	// them pretty-printed spends context on whitespace
	if (skills.is_array())
	{
		for (const json &skill : skills)
		{
			system += "Skill '" + string_field(skill, "id", "") + "': " + compact(skill) + "\n";
		}
	}

	// stated twice, positively then by failure mode. small models comply with
	// the shape far more reliably when the instruction is not a single clause.
	system += "Reply with exactly one JSON object and nothing else. No markdown fences, "
			  "no commentary before or after. Required shape: ";
	system += prompt_handoff_shape;
	system += "\n";
	system += "Use status 'approved' when your stage succeeded, 'revision' when the previous "
			  "stage must be redone, and 'blocked' when you cannot proceed. Put concrete "
			  "blocking problems in findings.";

	bool cut = false;
	std::string artifacts_text = pretty(artifacts);
	if (max_context_chars > 0)
	{
		artifacts_text =
		  truncate_characters(artifacts_text, static_cast<std::size_t>(max_context_chars), cut);
		if (cut)
			artifacts_text += "\n... truncated at " + std::to_string(max_context_chars) +
							  " characters ...";
	}

	// the stage is compact and the task is pretty on purpose: the stage is
	// machine context the model only has to match, the task is what it must
	// actually read.
	std::string user;
	user += "Stage: " + compact(stage) + "\n\n";
	user += "Task:\n" + pretty(task) + "\n\n";
	user += "Artifacts from previous stages:\n" + artifacts_text;

	return prompt_messages{system, user};
}

std::string prompt_build_single(const json &request, int max_context_chars)
{
	const prompt_messages messages = prompt_build(request, max_context_chars);
	return messages.system + "\n\n" + messages.user;
}
