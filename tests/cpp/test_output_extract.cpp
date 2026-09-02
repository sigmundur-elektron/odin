#include <doctest/doctest.h>

#include "output_extract.h"
#include "prompt_builder.h"

#include <string>

// ---------------------------------------------------------------- extraction

static json extracted(const std::string &text)
{
	json object;
	odin_error err;
	if (!output_extract_object(text, object, err))
		return json(nullptr);
	return object;
}

TEST_CASE("a bare object is taken as-is")
{
	CHECK(extracted(R"({"status":"approved"})").at("status") == "approved");
	CHECK(extracted("  \n\t{\"a\":1}\n  ").at("a") == 1);
}

TEST_CASE("a fenced block is unwrapped, whatever its language tag")
{
	CHECK(extracted("```json\n{\"a\":1}\n```").at("a") == 1);
	CHECK(extracted("```\n{\"a\":2}\n```").at("a") == 2);
	CHECK(extracted("Here you go:\n```JSON\n{\"a\":3}\n```\nHope that helps!").at("a") == 3);
}

TEST_CASE("an object is recovered from surrounding prose")
{
	CHECK(extracted("Sure! {\"a\":1} — let me know if you need changes.").at("a") == 1);
	CHECK(extracted("Thinking...\nDone.\n{\"status\":\"blocked\"}").at("status") == "blocked");
}

TEST_CASE("the LAST balanced span wins")
{
	// this is the case the ordering exists for: a model restates the requested
	// shape as an example, then answers. taking the first match would return
	// the example on every reply.
	const std::string reply =
	  "I should return {\"status\": \"approved\" | \"revision\"} like this.\n"
	  "Here is my answer:\n"
	  "{\"status\":\"revision\",\"summary\":\"needs work\"}";
	const json object = extracted(reply);
	CHECK(object.at("status") == "revision");
	CHECK(object.at("summary") == "needs work");
}

TEST_CASE("a fenced block still beats a later balanced span")
{
	// fences are an explicit signal; braces in prose are an inference
	CHECK(extracted("```json\n{\"a\":\"fenced\"}\n```\ntrailing {\"a\":\"loose\"}").at("a") ==
		  "fenced");
}

TEST_CASE("braces inside strings do not open or close a span")
{
	const json object = extracted(R"({"summary":"use {} and \"quotes\" freely","a":1})");
	CHECK(object.at("summary") == "use {} and \"quotes\" freely");
	CHECK(object.at("a") == 1);
}

TEST_CASE("output_balanced_spans finds only top-level regions")
{
	const std::string text = "{\"a\":{\"b\":1}} and {\"c\":2}";
	const auto spans = output_balanced_spans(text);
	REQUIRE(spans.size() == 2);
	CHECK(text.substr(spans[0].first, spans[0].second - spans[0].first) == "{\"a\":{\"b\":1}}");
	CHECK(text.substr(spans[1].first, spans[1].second - spans[1].first) == "{\"c\":2}");
}

TEST_CASE("an unbalanced fence keeps whatever was collected")
{
	CHECK(output_fenced_blocks("```json\n{\"a\":1}\n").empty());
	CHECK(output_fenced_blocks("no fences here").empty());
}

TEST_CASE("unicode whitespace is stripped, not left to break the parse")
{
	// U+2028 LINE SEPARATOR and U+3000 IDEOGRAPHIC SPACE. an ASCII-only trim
	// would leave these and the parse would fail on otherwise valid output.
	CHECK(extracted("\xe2\x80\xa8{\"a\":1}\xe3\x80\x80").at("a") == 1);
}

TEST_CASE("a non-object JSON value is not accepted as a handoff")
{
	CHECK(extracted("[1,2,3]").is_null());
	CHECK(extracted("\"just a string\"").is_null());
	CHECK(extracted("42").is_null());
}

TEST_CASE("failure names the problem and previews the output safely")
{
	odin_error err;
	json object;

	CHECK_FALSE(output_extract_object("", object, err));
	CHECK(err.message == "model returned no output");

	odin_error blank;
	CHECK_FALSE(output_extract_object("   \n  ", object, blank));
	CHECK(blank.message == "model returned no output");

	odin_error prose;
	CHECK_FALSE(output_extract_object("I cannot help with that.", object, prose));
	CHECK(prose.message.find("no JSON object found") != std::string::npos);
	CHECK(prose.message.find("I cannot help with that.") != std::string::npos);
}

TEST_CASE("the failure preview is bounded and never splits a character")
{
	// the message reaches durable state through a blocked stage's summary, so
	// a byte-sliced preview could write invalid UTF-8 into a JSON file
	std::string long_reply;
	for (int at = 0; at < 400; ++at)
		long_reply += "\xc3\xa9";

	odin_error err;
	json object;
	CHECK_FALSE(output_extract_object(long_reply, object, err));
	// still serialisable, i.e. the preview is valid UTF-8
	CHECK(json(err.message).dump().size() > 0);
	CHECK(err.message.size() < long_reply.size());
}

// --------------------------------------------------------------------- jsonl

TEST_CASE("a JSONL event stream is reassembled along a dotted path")
{
	const std::string stream =
	  "{\"part\":{\"text\":\"{\\\"status\\\":\"}}\n"
	  "{\"part\":{\"text\":\"\\\"approved\\\"}\"}}\n";
	CHECK(output_concat_event_text(stream, "part.text") == "{\"status\":\"approved\"}");
}

TEST_CASE("event lines that do not match are skipped, not fatal")
{
	const std::string stream = "progress: 40%\n"
							   "{\"part\":{\"text\":\"a\"}}\n"
							   "not json at all {\n"
							   "{\"other\":{\"field\":\"ignored\"}}\n"
							   "{\"part\":{\"text\":\"b\"}}\n";
	CHECK(output_concat_event_text(stream, "part.text") == "ab");
}

TEST_CASE("a non-string leaf contributes nothing")
{
	CHECK(output_concat_event_text("{\"part\":{\"text\":42}}\n", "part.text").empty());
	CHECK(output_concat_event_text("{\"part\":{\"text\":null}}\n", "part.text").empty());
}

// -------------------------------------------------------------------- prompt

static json sample_request()
{
	json request;
	request["agent"] = json{{"id", "reviewer"},
							{"purpose", "Review the specification."},
							{"rules", json::array({"Do not edit files.", "Be specific."})}};
	request["skills"] = json::array({json{{"id", "review"}, {"procedure", json::array({"read"})}}});
	request["stage"] = json{{"id", "review-spec"}, {"kind", "agent"}};
	request["task"] = json{{"id", "demo"}, {"request", "Add an export command."}};
	request["artifacts"] = json{{"specification", "some text"}};
	return request;
}

TEST_CASE("the prompt states the required shape and the status vocabulary")
{
	const prompt_messages messages = prompt_build(sample_request(), 24000);

	CHECK(messages.system.find("You are the 'reviewer' stage") != std::string::npos);
	CHECK(messages.system.find("Purpose: Review the specification.") != std::string::npos);
	CHECK(messages.system.find("- Do not edit files.") != std::string::npos);
	CHECK(messages.system.find("Skill 'review':") != std::string::npos);
	CHECK(messages.system.find(prompt_handoff_shape) != std::string::npos);
	// the vocabulary must be spelled out; a model told only the shape picks
	// plausible-looking statuses that are not in the enum
	CHECK(messages.system.find("'approved'") != std::string::npos);
	CHECK(messages.system.find("'revision'") != std::string::npos);
	CHECK(messages.system.find("'blocked'") != std::string::npos);

	CHECK(messages.user.find("Stage: ") != std::string::npos);
	CHECK(messages.user.find("Task:") != std::string::npos);
	CHECK(messages.user.find("Add an export command.") != std::string::npos);
	CHECK(messages.user.find("Artifacts from previous stages:") != std::string::npos);
}

TEST_CASE("the stage is compact and the task is pretty printed")
{
	const prompt_messages messages = prompt_build(sample_request(), 24000);
	// the stage is context to match; the task is what the model must read
	CHECK(messages.user.find("Stage: {\"id\":\"review-spec\",\"kind\":\"agent\"}") !=
		  std::string::npos);
	CHECK(messages.user.find("\n  \"request\": \"Add an export command.\"") != std::string::npos);
}

TEST_CASE("an agent with no rules omits the Rules heading")
{
	json request = sample_request();
	request["agent"].erase("rules");
	const prompt_messages messages = prompt_build(request, 24000);
	CHECK(messages.system.find("Rules:") == std::string::npos);
}

TEST_CASE("artifacts are truncated on a character boundary")
{
	json request = sample_request();
	std::string wide;
	for (int at = 0; at < 500; ++at)
		wide += "\xc3\xa9";
	request["artifacts"] = json{{"blob", wide}};

	const prompt_messages messages = prompt_build(request, 100);
	CHECK(messages.user.find("... truncated at 100 characters ...") != std::string::npos);
	// the whole prompt must still be valid UTF-8, or the request body cannot
	// be serialised at all
	CHECK(json(messages.user).dump().size() > 0);
}

TEST_CASE("a request missing fields still produces a usable prompt")
{
	// a malformed request must not crash the adapter before it can report
	const prompt_messages messages = prompt_build(json::object(), 24000);
	CHECK(messages.system.find("You are the 'agent' stage") != std::string::npos);
	CHECK_FALSE(messages.user.empty());
}

TEST_CASE("the single-string prompt carries both halves")
{
	const std::string single = prompt_build_single(sample_request(), 24000);
	CHECK(single.find("You are the 'reviewer' stage") != std::string::npos);
	CHECK(single.find("Artifacts from previous stages:") != std::string::npos);
}
