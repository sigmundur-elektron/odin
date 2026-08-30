"""Regenerate the json byte-parity corpus consumed by tests/cpp/test_json_parity.cpp.

The C++ port has to reproduce `harness/io.py`'s output exactly, because a GUI and
both implementations read the same state files. Rather than hand-copy fixtures,
this records what CPython actually produces for a spread of awkward values.

Run from the repository root after touching json_io.cpp or upgrading nlohmann:

    python scripts/make_parity_corpus.py

The `expected` field holds the exact bytes `write_json_atomic` would leave on
disk on THIS platform, including the CRLF that text-mode writing introduces on
Windows. Regenerate per platform; the C++ side applies the same rule.
"""

from __future__ import annotations

import io
import json
import os
from pathlib import Path

FIXTURE = Path(__file__).resolve().parent.parent / "tests" / "fixtures" / "json_parity.json"

# Each case is (name, value). Names are documentation for a failing assertion.
CASES: list[tuple[str, object]] = [
    ("empty object", {}),
    ("empty nested containers", {"object": {}, "array": []}),
    ("key ordering", {"zebra": 1, "Alpha": 2, "_under": 3, "10": 4, "2": 5}),
    ("integers", {"zero": 0, "negative": -17, "large": 9007199254740993}),
    # a whole-valued float must stay 1.0; collapsing it to 1 breaks parity
    ("floats", {"whole": 1.0, "zero": 0.0, "negative": -2.5, "small": 1e-07, "big": 1e300}),
    ("booleans and null", {"yes": True, "no": False, "nothing": None}),
    ("latin-1 range", {"text": "caf\u00e9 na\u00efve \u00fcber"}),
    ("cjk", {"text": "\u4f60\u597d\u4e16\u754c"}),
    # astral plane: python emits a surrogate pair under ensure_ascii
    ("astral plane", {"text": "\U0001f600 \U0001f4a9"}),
    ("escapes", {"text": "quote \" backslash \\ slash /"}),
    ("whitespace controls", {"text": "tab\there\nnewline\r\nreturn\bback\fform"}),
    # 0x7f is ascii but outside python's ' '..'~' safe range, so python escapes it
    ("low and high controls", {"text": "\x01\x1f\x7f"}),
    ("unicode keys", {"\u00e9": 1, "\u4f60": 2, "plain": 3}),
    ("deep nesting", {"a": {"b": {"c": {"d": [1, [2, [3, {"e": None}]]]}}}}),
    (
        "handoff shaped",
        {
            "status": "approved",
            "summary": "gate 'quality' exited 0",
            "artifacts": {"changed_files": ["src/a.cpp", "src/b.h"], "gate": {"exit_code": 0}},
            "findings": [],
        },
    ),
]

# Every subprocess call in the harness decodes with errors="replace", and the
# result goes straight into a state file. Getting the number of U+FFFD wrong
# would corrupt an artifact, so the exact behaviour is pinned here rather than
# assumed: CPython follows the Unicode "maximal subpart" rule, which is not the
# same as one replacement per bad byte.
UTF8_CASES: list[tuple[str, bytes]] = [
    ("valid ascii", b"hello"),
    ("valid cjk", b"\xe4\xbd\xa0\xe5\xa5\xbd"),
    ("valid astral", b"\xf0\x9f\x98\x80"),
    ("lone invalid byte", b"\xff"),
    ("latin-1 masquerading as utf-8", b"caf\xe9"),
    ("truncated two byte", b"\xc3"),
    ("truncated three byte", b"\xe4\xbd"),
    ("truncated four byte", b"\xf0\x9f\x98"),
    ("invalid byte between valid text", b"ok\xffmore"),
    ("surrogate half", b"\xed\xa0\x80"),
    ("overlong nul", b"\xc0\x80"),
    ("overlong slash", b"\xe0\x80\xaf"),
    ("beyond u+10ffff", b"\xf4\x90\x80\x80"),
    ("continuation without lead", b"\x80\x80"),
    ("two truncated sequences", b"\xe4\xbd\xe4\xbd"),
    ("gate output with a bad byte", b"FAILED tests/x.py::test_a\n  assert \xe9 == 1\n"),
]


def python_bytes(value: object) -> str:
    """Exactly what harness/io.py writes, including text-mode newline handling."""
    stream = io.StringIO()
    json.dump(value, stream, indent=2, sort_keys=True)
    stream.write("\n")
    return stream.getvalue().replace("\n", os.linesep)


def main() -> int:
    corpus = {
        "linesep": "crlf" if os.linesep == "\r\n" else "lf",
        "cases": [
            {"name": name, "value": value, "expected": python_bytes(value)}
            for name, value in CASES
        ],
        "utf8_cases": [
            {"name": name, "bytes": list(raw), "expected": raw.decode("utf-8", errors="replace")}
            for name, raw in UTF8_CASES
        ],
    }
    FIXTURE.parent.mkdir(parents=True, exist_ok=True)
    FIXTURE.write_text(json.dumps(corpus, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {len(CASES)} json cases and {len(UTF8_CASES)} utf-8 cases to {FIXTURE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
