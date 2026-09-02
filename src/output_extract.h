#pragma once
#include <string>
#include <vector>

#include "types.h"

// Tolerant JSON-object recovery from model output.
//
// Local and hosted models routinely wrap structured output in prose, markdown
// fences, or reasoning preamble. Every adapter needs the same recovery, so it
// lives here once and is tested directly rather than reimplemented per provider.
//
// Recovery is not trust: whatever comes back is still validated against
// handoff.schema.json before it can enter a run's artifact history.

// Returns false with out_error set when nothing usable was found. The message
// carries a bounded preview of the output, sliced on character boundaries so a
// failure diagnostic cannot itself be invalid UTF-8.
bool output_extract_object(const std::string &text, json &out_object, odin_error &out_error);

// Concatenate text fields from a newline-delimited JSON event stream.
//
// CLI agents that stream events rather than returning one blob are common;
// this reassembles the assistant text given only a dotted field path, so the
// adapter needs no other provider-specific knowledge.
std::string output_concat_event_text(const std::string &stream, const std::string &text_path);

// --- exposed for tests -----------------------------------------------------

// Byte offsets of top-level {...} regions, string-aware: a brace inside a JSON
// string must not open or close a span.
std::vector<std::pair<std::size_t, std::size_t>> output_balanced_spans(const std::string &text);

// Contents of ``` fenced blocks, ignoring the language tag.
std::vector<std::string> output_fenced_blocks(const std::string &text);
