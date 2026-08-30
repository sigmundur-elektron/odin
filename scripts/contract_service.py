"""Long-lived contract validator, driven by the C++ harness over stdin/stdout.

`jsonschema` is odin's only third-party dependency and the only part of the
harness that a C++ rewrite cannot cheaply reproduce: draft 2020-12 with `if`/
`then` and `pattern`. Rather than reimplement it, the C++ engine keeps one of
these processes alive and asks it.

Validation happens once per stage, so a one-shot subprocess per call would spend
roughly twenty seconds per run on process startup alone. In here it is about a
millisecond.

Protocol
--------
One JSON object per line, in both directions. Requests:

    {"id": 1, "op": "ping"}
    {"id": 2, "op": "validate", "contract": "handoff",
     "where": "stage 'review' output", "value": {...}}
    {"id": 3, "op": "shutdown"}

Replies:

    {"id": 1, "ok": true}
    {"id": 2, "ok": false, "error": "stage 'review' output violates handoff: ..."}

Framing is safe because every reply is written with ensure_ascii=True, so a
literal newline can never appear inside a payload.

This module deliberately calls `harness.contracts.validate` rather than talking
to jsonschema itself: the error text is user-facing product surface, and routing
both implementations through one function is what keeps it identical.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

# the adapters do the same, so the script stays runnable from anywhere
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from harness.contracts import validate  # noqa: E402
from harness.errors import OdinError  # noqa: E402


def handle(request: dict) -> dict:
    identifier = request.get("id")
    operation = request.get("op")

    if operation == "ping":
        return {"id": identifier, "ok": True}

    if operation == "validate":
        contract = request.get("contract")
        if not isinstance(contract, str):
            return {"id": identifier, "ok": False, "error": "validate requires a 'contract' name"}
        where = request.get("where") or "value"
        try:
            validate(request.get("value"), contract, where)
        except OdinError as error:
            return {"id": identifier, "ok": False, "error": str(error)}
        return {"id": identifier, "ok": True}

    return {"id": identifier, "ok": False, "error": f"unknown op: {operation!r}"}


def main() -> int:
    # stdout is the protocol channel and nothing else may touch it. anything a
    # library decides to print goes to stderr, where the parent captures it for
    # diagnostics.
    protocol = sys.stdout
    sys.stdout = sys.stderr

    for line in protocol_lines():
        try:
            request = json.loads(line)
        except json.JSONDecodeError as error:
            reply = {"id": None, "ok": False, "error": f"malformed request: {error}"}
        else:
            if request.get("op") == "shutdown":
                return 0
            reply = handle(request)

        protocol.write(json.dumps(reply, ensure_ascii=True, separators=(",", ":")))
        protocol.write("\n")
        protocol.flush()
    return 0


def protocol_lines():
    for raw in sys.stdin:
        line = raw.strip()
        if line:
            yield line


if __name__ == "__main__":
    raise SystemExit(main())
