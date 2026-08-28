"""Tolerant JSON-object extraction from model output.

Local and hosted models frequently wrap structured output in prose, markdown
fences, or reasoning preamble. Every adapter needs the same recovery logic, so
it lives here once and is unit tested rather than reimplemented per provider.
"""

from __future__ import annotations

import json
from typing import Any


class ExtractionError(ValueError):
    """No JSON object could be recovered from the text."""


def _balanced_spans(text: str) -> list[tuple[int, int]]:
    """Return (start, end) spans of top-level {...} regions, string-aware."""
    spans: list[tuple[int, int]] = []
    depth = 0
    start = -1
    in_string = False
    escaped = False
    for index, character in enumerate(text):
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character == "{":
            if depth == 0:
                start = index
            depth += 1
        elif character == "}":
            if depth > 0:
                depth -= 1
                if depth == 0 and start >= 0:
                    spans.append((start, index + 1))
    return spans


def _fenced_blocks(text: str) -> list[str]:
    """Return the contents of ``` fenced blocks, ignoring the language tag."""
    blocks: list[str] = []
    marker = "```"
    position = 0
    while True:
        opening = text.find(marker, position)
        if opening == -1:
            return blocks
        newline = text.find("\n", opening)
        if newline == -1:
            return blocks
        closing = text.find(marker, newline)
        if closing == -1:
            return blocks
        blocks.append(text[newline + 1 : closing])
        position = closing + len(marker)


def extract_json_object(text: str) -> dict[str, Any]:
    """Recover a single JSON object from arbitrary model output.

    Tries, in order: the whole string, fenced code blocks, then every balanced
    brace span. The last balanced span wins when several parse, because models
    that restate an example before answering put the real answer last.
    """
    if not text or not text.strip():
        raise ExtractionError("model returned no output")

    candidates: list[str] = [text.strip()]
    candidates.extend(block.strip() for block in _fenced_blocks(text))

    for candidate in candidates:
        try:
            value = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            return value

    recovered: dict[str, Any] | None = None
    for start, end in _balanced_spans(text):
        try:
            value = json.loads(text[start:end])
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            recovered = value
    if recovered is not None:
        return recovered

    preview = text.strip()[:200].replace("\n", " ")
    raise ExtractionError(f"no JSON object found in model output: {preview}")


def concat_event_text(stream: str, text_path: str = "part.text") -> str:
    """Concatenate text fields from a newline-delimited JSON event stream.

    CLI agents that stream structured events (rather than returning one blob)
    are common; this reassembles their assistant text without the adapter
    needing provider-specific knowledge beyond the field path.
    """
    parts: list[str] = []
    keys = text_path.split(".")
    for line in stream.splitlines():
        line = line.strip()
        if not line or not line.startswith("{"):
            continue
        try:
            event: Any = json.loads(line)
        except json.JSONDecodeError:
            continue
        for key in keys:
            if not isinstance(event, dict):
                event = None
                break
            event = event.get(key)
        if isinstance(event, str):
            parts.append(event)
    return "".join(parts)
