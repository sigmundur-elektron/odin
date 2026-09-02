#include <doctest/doctest.h>

#include "subprocess.h"

#include <initializer_list>
#include <string>

// utf8_sanitize is the decoder every captured subprocess stream passes through,
// and its output lands in durable JSON state. A raw invalid byte written into a
// state file produces an artifact nothing can read back, so this is a data
// integrity check rather than a formatting one.
//
// These cases were previously asserted against a generated CPython corpus. They
// are stated directly here instead: the behaviour is fixed by Unicode, not by
// Python, and the corpus made a decoder test depend on an interpreter and on
// which platform generated the fixture.
//
// The rule being enforced is the Unicode "maximal subpart" substitution policy
// (Unicode 16.0 section 3.9, W3C-recommended practice): on encountering an
// ill-formed sequence, emit one U+FFFD for the longest prefix that could still
// have begun a valid sequence, then resume. The consequence is unintuitive and
// is exactly what makes these cases worth pinning - b"\xe0\x80\xaf" yields three
// replacements, not one, because neither continuation byte is admissible after
// the overlong lead.

namespace
{

std::string bytes(std::initializer_list<int> values)
{
	std::string raw;
	for (const int value : values)
		raw.push_back(static_cast<char>(value));
	return raw;
}

// U+FFFD REPLACEMENT CHARACTER, encoded
const std::string R = "\xef\xbf\xbd";

} // namespace

TEST_CASE("utf8_sanitize passes well formed text through unchanged")
{
	CHECK(utf8_sanitize("hello") == "hello");
	// CJK, three bytes per code point
	CHECK(utf8_sanitize(bytes({0xe4, 0xbd, 0xa0, 0xe5, 0xa5, 0xbd})) ==
		  bytes({0xe4, 0xbd, 0xa0, 0xe5, 0xa5, 0xbd}));
	// astral plane, four bytes
	CHECK(utf8_sanitize(bytes({0xf0, 0x9f, 0x98, 0x80})) == bytes({0xf0, 0x9f, 0x98, 0x80}));
	CHECK(utf8_sanitize("") == "");
}

TEST_CASE("utf8_sanitize replaces a single ill formed byte")
{
	CHECK(utf8_sanitize(bytes({0xff})) == R);
	// latin-1 text mislabelled as utf-8: the most common real cause
	CHECK(utf8_sanitize(bytes({0x63, 0x61, 0x66, 0xe9})) == "caf" + R);
	// surrounding valid text must survive intact
	CHECK(utf8_sanitize(bytes({0x6f, 0x6b, 0xff, 0x6d, 0x6f, 0x72, 0x65})) == "ok" + R + "more");
}

TEST_CASE("utf8_sanitize collapses a truncated sequence to one replacement")
{
	// each of these is a valid *prefix*, so the whole prefix is one maximal
	// subpart and yields exactly one U+FFFD
	CHECK(utf8_sanitize(bytes({0xc3})) == R);
	CHECK(utf8_sanitize(bytes({0xe4, 0xbd})) == R);
	CHECK(utf8_sanitize(bytes({0xf0, 0x9f, 0x98})) == R);
	// two truncated sequences are two subparts, not one run
	CHECK(utf8_sanitize(bytes({0xe4, 0xbd, 0xe4, 0xbd})) == R + R);
}

TEST_CASE("utf8_sanitize emits one replacement per byte of a rejected sequence")
{
	// the counter-intuitive half of the maximal subpart rule. in each case the
	// lead byte is only ever valid with continuations these bytes cannot
	// supply, so no byte joins a subpart and each is replaced on its own.

	SUBCASE("a surrogate half, which utf-8 may not encode")
	{
		CHECK(utf8_sanitize(bytes({0xed, 0xa0, 0x80})) == R + R + R);
	}

	SUBCASE("overlong encodings, the classic security bypass")
	{
		// an overlong NUL and an overlong '/' - both decode to an ASCII
		// character that a naive decoder would let through a path check
		CHECK(utf8_sanitize(bytes({0xc0, 0x80})) == R + R);
		CHECK(utf8_sanitize(bytes({0xe0, 0x80, 0xaf})) == R + R + R);
	}

	SUBCASE("a code point beyond U+10FFFF")
	{
		CHECK(utf8_sanitize(bytes({0xf4, 0x90, 0x80, 0x80})) == R + R + R + R);
	}

	SUBCASE("continuation bytes with no lead")
	{
		CHECK(utf8_sanitize(bytes({0x80, 0x80})) == R + R);
	}
}

TEST_CASE("utf8_sanitize keeps a gate's output readable around a bad byte")
{
	// the shape this actually protects: a project gate prints a test failure
	// containing one non-utf-8 byte, and the whole report has to survive into
	// context.json rather than being dropped or corrupting the file.
	const std::string raw = "FAILED tests/x.py::test_a\n  assert " + bytes({0xe9}) + " == 1\n";
	CHECK(utf8_sanitize(raw) == "FAILED tests/x.py::test_a\n  assert " + R + " == 1\n");
}
