#pragma once
#include <string>

#include "types.h"

// Canonical agent prompt construction.
//
// The wording here is a product contract, not an implementation detail: it is
// what makes a small local model return a parseable handoff instead of prose.
// Both built-in adapters render the same text, so switching transport does not
// silently change model behaviour.

// The shape the model is told to produce, quoted verbatim into the prompt.
extern const char *const prompt_handoff_shape;

struct prompt_messages
{
	std::string system;
	std::string user;
};

// `request` is the agent request object: agent, skills, stage, task, artifacts.
//
// Artifacts are truncated to max_context_chars on a character boundary; a byte
// truncation could split a UTF-8 sequence and produce a prompt that is not
// valid text.
prompt_messages prompt_build(const json &request, int max_context_chars);

// A single prompt string, for CLI agents that take one argument or one stdin
// blob rather than a role-tagged message list.
std::string prompt_build_single(const json &request, int max_context_chars);
