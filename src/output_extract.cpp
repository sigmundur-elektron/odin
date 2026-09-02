#include "output_extract.h"

#include <cstddef>

namespace
{

// Unicode whitespace, not just ASCII. Model output frequently carries NBSP and
// typographic spaces, and std::isspace would leave them in place - which turns a
// perfectly good JSON object into one that fails to parse.
bool is_space(const std::string &text, std::size_t at, std::size_t &out_width)
{
	const unsigned char first = static_cast<unsigned char>(text[at]);
	out_width = 1;
	if (first == ' ' || first == '\t' || first == '\n' || first == '\r' || first == '\f' ||
		first == '\v')
		return true;

	// U+2000..U+200A, U+2028, U+2029, U+205F, U+3000 are all encoded as three
	// bytes beginning 0xE2 or 0xE3
	if (at + 2 < text.size() && first == 0xE2)
	{
		const unsigned char second = static_cast<unsigned char>(text[at + 1]);
		const unsigned char third = static_cast<unsigned char>(text[at + 2]);
		if (second == 0x80 && ((third >= 0x80 && third <= 0x8A) || third == 0xA8 || third == 0xA9))
		{
			out_width = 3;
			return true;
		}
		if (second == 0x81 && third == 0x9F)
		{
			out_width = 3;
			return true;
		}
	}
	if (at + 2 < text.size() && first == 0xE3 &&
		static_cast<unsigned char>(text[at + 1]) == 0x80 &&
		static_cast<unsigned char>(text[at + 2]) == 0x80)
	{
		out_width = 3;
		return true;
	}
	return false;
}

std::string trimmed(const std::string &text)
{
	std::size_t start = 0;
	std::size_t width = 0;
	while (start < text.size() && is_space(text, start, width))
		start += width;

	std::size_t end = text.size();
	while (end > start)
	{
		// step back to the start of the previous character
		std::size_t previous = end - 1;
		while (previous > start && (static_cast<unsigned char>(text[previous]) & 0xC0) == 0x80)
			--previous;
		if (!is_space(text, previous, width))
			break;
		end = previous;
	}
	return text.substr(start, end - start);
}

// truncate to at most `characters` code points, never mid-sequence
std::string preview_of(const std::string &text, std::size_t characters)
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
	std::string preview = text.substr(0, at);
	for (char &character : preview)
	{
		if (character == '\n' || character == '\r')
			character = ' ';
	}
	return preview;
}

bool parse_object(const std::string &candidate, json &out_object)
{
	const json value = json::parse(candidate, nullptr, false);
	if (value.is_discarded() || !value.is_object())
		return false;
	out_object = value;
	return true;
}

} // namespace

std::vector<std::pair<std::size_t, std::size_t>> output_balanced_spans(const std::string &text)
{
	std::vector<std::pair<std::size_t, std::size_t>> spans;
	int depth = 0;
	std::size_t start = 0;
	bool have_start = false;
	bool in_string = false;
	bool escaped = false;

	for (std::size_t at = 0; at < text.size(); ++at)
	{
		const char character = text[at];
		if (in_string)
		{
			// the escape state is what stops a \" inside a string from being
			// read as the string's end, and a brace after it from being counted
			if (escaped)
				escaped = false;
			else if (character == '\\')
				escaped = true;
			else if (character == '"')
				in_string = false;
			continue;
		}
		if (character == '"')
		{
			in_string = true;
		}
		else if (character == '{')
		{
			if (depth == 0)
			{
				start = at;
				have_start = true;
			}
			++depth;
		}
		else if (character == '}')
		{
			if (depth > 0)
			{
				--depth;
				if (depth == 0 && have_start)
					spans.push_back({start, at + 1});
			}
		}
	}
	return spans;
}

std::vector<std::string> output_fenced_blocks(const std::string &text)
{
	std::vector<std::string> blocks;
	const std::string marker = "```";
	std::size_t position = 0;

	for (;;)
	{
		const std::size_t opening = text.find(marker, position);
		if (opening == std::string::npos)
			return blocks;
		// everything up to the first newline is the language tag, whatever it
		// says, so it is skipped rather than interpreted
		const std::size_t newline = text.find('\n', opening);
		if (newline == std::string::npos)
			return blocks;
		const std::size_t closing = text.find(marker, newline);
		if (closing == std::string::npos)
			return blocks; // unbalanced fence: keep what was collected
		blocks.push_back(text.substr(newline + 1, closing - newline - 1));
		position = closing + marker.size();
	}
}

bool output_extract_object(const std::string &text, json &out_object, odin_error &out_error)
{
	const std::string whole = trimmed(text);
	if (whole.empty())
	{
		fail(out_error, error_kind::adapter, "model returned no output");
		return false;
	}

	// tier one and two: the whole reply, then each fenced block in document
	// order. the first that parses as an object wins.
	if (parse_object(whole, out_object))
		return true;
	for (const std::string &block : output_fenced_blocks(text))
	{
		if (parse_object(trimmed(block), out_object))
			return true;
	}

	// tier three: every balanced brace span, and here the LAST one wins.
	// deliberate - a model that restates the requested shape as an example
	// before answering puts the real answer last, so taking the first would
	// reliably return the example.
	bool recovered = false;
	json candidate;
	for (const auto &[start, end] : output_balanced_spans(text))
	{
		if (parse_object(text.substr(start, end - start), candidate))
		{
			out_object = candidate;
			recovered = true;
		}
	}
	if (recovered)
		return true;

	fail(out_error, error_kind::adapter,
		 "no JSON object found in model output: " + preview_of(whole, 200));
	return false;
}

std::string output_concat_event_text(const std::string &stream, const std::string &text_path)
{
	std::vector<std::string> keys;
	std::size_t start = 0;
	for (;;)
	{
		const std::size_t dot = text_path.find('.', start);
		if (dot == std::string::npos)
		{
			if (start < text_path.size())
				keys.push_back(text_path.substr(start));
			break;
		}
		keys.push_back(text_path.substr(start, dot - start));
		start = dot + 1;
	}

	std::string joined;
	std::size_t at = 0;
	while (at <= stream.size())
	{
		const std::size_t end = stream.find('\n', at);
		const std::string line =
		  trimmed(stream.substr(at, end == std::string::npos ? std::string::npos : end - at));

		// a stream carries progress lines and other noise; anything that is not
		// an object is skipped rather than treated as a failure
		if (!line.empty() && line[0] == '{')
		{
			json event = json::parse(line, nullptr, false);
			bool usable = !event.is_discarded();
			for (const std::string &key : keys)
			{
				if (!usable || !event.is_object())
				{
					usable = false;
					break;
				}
				const auto found = event.find(key);
				if (found == event.end())
				{
					usable = false;
					break;
				}
				event = *found;
			}
			if (usable && event.is_string())
				joined += event.get<std::string>();
		}

		if (end == std::string::npos)
			break;
		at = end + 1;
	}
	return joined;
}
