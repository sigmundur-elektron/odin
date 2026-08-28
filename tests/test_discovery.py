from __future__ import annotations

import unittest

from harness.discovery import (
    DiscoveredModel,
    DiscoveredProvider,
    emit_config,
    parse_ollama_tags,
    parse_openai_models,
    parse_parameter_billions,
)


class ParseOpenAiModelsTests(unittest.TestCase):
    def test_extracts_and_sorts_ids(self) -> None:
        payload = {"object": "list", "data": [{"id": "b"}, {"id": "a"}]}
        self.assertEqual(parse_openai_models(payload), ["a", "b"])

    def test_tolerates_missing_and_malformed_entries(self) -> None:
        payload = {"data": [{"id": "a"}, {"no_id": 1}, "junk", None]}
        self.assertEqual(parse_openai_models(payload), ["a"])

    def test_non_dict_payload(self) -> None:
        self.assertEqual(parse_openai_models(["a"]), [])


class ParseParameterBillionsTests(unittest.TestCase):
    def test_parses_decimal_and_integer_sizes(self) -> None:
        self.assertEqual(parse_parameter_billions("32.8B"), 32.8)
        self.assertEqual(parse_parameter_billions("7B"), 7.0)

    def test_rejects_unparseable(self) -> None:
        self.assertIsNone(parse_parameter_billions("unknown"))
        self.assertIsNone(parse_parameter_billions(None))


class ParseOllamaTagsTests(unittest.TestCase):
    def test_captures_size_and_quantization(self) -> None:
        payload = {
            "models": [
                {
                    "name": "qwen2.5-coder:32b",
                    "details": {
                        "parameter_size": "32.8B",
                        "quantization_level": "Q4_K_M",
                        "family": "qwen2",
                    },
                }
            ]
        }
        models = parse_ollama_tags(payload)
        self.assertEqual(len(models), 1)
        self.assertEqual(models[0].id, "qwen2.5-coder:32b")
        self.assertEqual(models[0].parameter_billions, 32.8)
        self.assertEqual(models[0].detail["quantization"], "Q4_K_M")

    def test_missing_details_is_not_fatal(self) -> None:
        models = parse_ollama_tags({"models": [{"name": "x"}]})
        self.assertEqual(models[0].id, "x")
        self.assertIsNone(models[0].parameter_billions)


class EmitConfigTests(unittest.TestCase):
    def test_emits_adapter_and_profile_for_http_provider(self) -> None:
        provider = DiscoveredProvider(
            name="ollama",
            transport="openai-compatible",
            status="ready",
            base_url="http://127.0.0.1:11434/v1",
            models=[
                DiscoveredModel(
                    id="qwen2.5-coder:32b",
                    provider="ollama",
                    transport="openai-compatible",
                    base_url="http://127.0.0.1:11434/v1",
                    parameter_billions=32.8,
                )
            ],
        )
        rendered = emit_config([provider])
        self.assertIn("[adapters.http-ollama]", rendered)
        self.assertIn("adapters/openai_compatible.py", rendered)
        self.assertIn('"--base-url", "http://127.0.0.1:11434/v1"', rendered)
        self.assertIn("[models.qwen2-5-coder-32b]", rendered)
        self.assertIn("parameter_billions = 32.8", rendered)

    def test_unready_providers_are_skipped(self) -> None:
        provider = DiscoveredProvider(name="vllm", transport="openai-compatible", status="error")
        self.assertIn("No reachable providers", emit_config([provider]))

    def test_api_key_is_referenced_by_name_not_value(self) -> None:
        provider = DiscoveredProvider(
            name="openrouter",
            transport="openai-compatible",
            status="ready",
            base_url="https://openrouter.ai/api/v1",
            api_key_env="OPENROUTER_API_KEY",
            models=[
                DiscoveredModel(
                    id="anthropic/claude-sonnet-4.5",
                    provider="openrouter",
                    transport="openai-compatible",
                    base_url="https://openrouter.ai/api/v1",
                    api_key_env="OPENROUTER_API_KEY",
                )
            ],
        )
        rendered = emit_config([provider])
        self.assertIn('"--api-key-env", "OPENROUTER_API_KEY"', rendered)
        self.assertIn("[models.anthropic-claude-sonnet-4-5]", rendered)


if __name__ == "__main__":
    unittest.main()
