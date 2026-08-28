from __future__ import annotations

import unittest

from harness.extract import ExtractionError, concat_event_text, extract_json_object


HANDOFF = {"status": "approved", "summary": "ok", "artifacts": {}, "findings": []}


class ExtractJsonObjectTests(unittest.TestCase):
    def test_bare_object(self) -> None:
        self.assertEqual(extract_json_object('{"status": "approved"}'), {"status": "approved"})

    def test_markdown_fence_with_language_tag(self) -> None:
        text = 'Here you go:\n```json\n{"status": "approved"}\n```\nHope that helps.'
        self.assertEqual(extract_json_object(text), {"status": "approved"})

    def test_unlabelled_fence(self) -> None:
        self.assertEqual(extract_json_object('```\n{"a": 1}\n```'), {"a": 1})

    def test_reasoning_preamble_then_object(self) -> None:
        text = 'Let me think. The stage passed.\n{"status": "approved", "findings": []}'
        self.assertEqual(
            extract_json_object(text), {"status": "approved", "findings": []}
        )

    def test_last_object_wins_when_example_is_restated_first(self) -> None:
        # Small models often echo the requested shape before answering.
        text = 'Shape: {"status": "approved|revision"} \nAnswer: {"status": "revision"}'
        self.assertEqual(extract_json_object(text), {"status": "revision"})

    def test_braces_inside_strings_do_not_break_scanning(self) -> None:
        text = '{"summary": "use {model} in the template", "status": "approved"}'
        self.assertEqual(extract_json_object(text)["summary"], "use {model} in the template")

    def test_escaped_quote_inside_string(self) -> None:
        text = r'{"summary": "he said \"done\"", "status": "approved"}'
        self.assertEqual(extract_json_object(text)["summary"], 'he said "done"')

    def test_nested_object_is_preserved(self) -> None:
        text = 'prose {"artifacts": {"changed_files": ["a.cpp"]}} trailing'
        self.assertEqual(
            extract_json_object(text), {"artifacts": {"changed_files": ["a.cpp"]}}
        )

    def test_empty_output_raises(self) -> None:
        with self.assertRaises(ExtractionError):
            extract_json_object("   ")

    def test_prose_without_json_raises(self) -> None:
        with self.assertRaises(ExtractionError):
            extract_json_object("I cannot help with that request.")

    def test_json_array_alone_is_not_an_object(self) -> None:
        with self.assertRaises(ExtractionError):
            extract_json_object("[1, 2, 3]")


class ConcatEventTextTests(unittest.TestCase):
    def test_concatenates_streamed_text_parts(self) -> None:
        stream = "\n".join(
            [
                '{"type":"step_start","part":{"type":"step-start"}}',
                '{"type":"text","part":{"type":"text","text":"{\\"status\\":"}}',
                '{"type":"text","part":{"type":"text","text":"\\"approved\\"}"}}',
                '{"type":"step_finish","part":{"type":"step-finish"}}',
            ]
        )
        self.assertEqual(concat_event_text(stream), '{"status":"approved"}')

    def test_ignores_malformed_lines(self) -> None:
        stream = 'not json\n{"part":{"text":"ok"}}\n\n'
        self.assertEqual(concat_event_text(stream), "ok")

    def test_custom_text_path(self) -> None:
        stream = '{"delta":{"content":"hello"}}'
        self.assertEqual(concat_event_text(stream, "delta.content"), "hello")

    def test_missing_path_yields_empty(self) -> None:
        self.assertEqual(concat_event_text('{"other":1}'), "")


if __name__ == "__main__":
    unittest.main()
