from __future__ import annotations

import json
import os
import stat
import tempfile
import unittest
from pathlib import Path

from harness.credentials import (
    CredentialError,
    CredentialStore,
    mask,
    resolve_secret,
)

SECRET = "sk-liveKEY1234567890abcdef"


class MaskTests(unittest.TestCase):
    def test_long_value_shows_only_edges(self) -> None:
        masked = mask(SECRET)
        self.assertNotIn("liveKEY1234567890", masked)
        self.assertTrue(masked.startswith("sk-"))
        self.assertTrue(masked.endswith("cdef"))

    def test_short_value_is_fully_hidden(self) -> None:
        self.assertEqual(mask("abc"), "***")

    def test_empty(self) -> None:
        self.assertEqual(mask(""), "<empty>")
        self.assertEqual(mask(None), "<empty>")


class CredentialStoreTests(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)
        self.store = CredentialStore(self.root)

    def tearDown(self) -> None:
        self._dir.cleanup()

    def test_round_trip(self) -> None:
        self.store.set_api_key("openrouter", SECRET)
        self.assertEqual(self.store.secret("openrouter"), SECRET)
        self.assertEqual(self.store.names(), ["openrouter"])

    def test_store_lives_under_dot_odin_which_is_gitignored(self) -> None:
        self.store.set_api_key("x", SECRET)
        self.assertEqual(self.store.path.parent.name, ".odin")
        self.assertTrue(self.store.path.exists())

    def test_describe_never_returns_the_raw_secret(self) -> None:
        self.store.set_api_key("openrouter", SECRET, note="for benchmarking")
        described = self.store.describe("openrouter")
        self.assertNotIn(SECRET, json.dumps(described))
        self.assertEqual(described["note"], "for benchmarking")
        self.assertEqual(described["type"], "api_key")

    def test_describe_all_is_safe_to_print(self) -> None:
        self.store.set_api_key("a", SECRET)
        self.store.set_api_key("b", "another-long-secret-value")
        rendered = json.dumps(self.store.describe_all())
        self.assertNotIn(SECRET, rendered)
        self.assertNotIn("another-long-secret-value", rendered)

    def test_missing_credential_names_the_fix(self) -> None:
        with self.assertRaises(CredentialError) as caught:
            self.store.secret("absent")
        self.assertIn("auth set absent", str(caught.exception))

    def test_empty_value_is_refused(self) -> None:
        with self.assertRaises(CredentialError):
            self.store.set_api_key("x", "   ")

    def test_remove(self) -> None:
        self.store.set_api_key("x", SECRET)
        self.assertTrue(self.store.remove("x"))
        self.assertFalse(self.store.remove("x"))
        self.assertEqual(self.store.names(), [])

    def test_overwrite_replaces_value(self) -> None:
        self.store.set_api_key("x", SECRET)
        self.store.set_api_key("x", "second-value-long-enough")
        self.assertEqual(self.store.secret("x"), "second-value-long-enough")

    @unittest.skipIf(os.name == "nt", "POSIX file modes only")
    def test_file_mode_is_owner_only_on_posix(self) -> None:
        self.store.set_api_key("x", SECRET)
        mode = stat.S_IMODE(self.store.path.stat().st_mode)
        self.assertEqual(mode, 0o600)

    def test_expired_oauth_is_reported_and_refused(self) -> None:
        self.store.set_oauth("copilot", "token-value-long", expires=0)
        self.assertTrue(self.store.is_expired("copilot"))
        with self.assertRaises(CredentialError) as caught:
            self.store.secret("copilot")
        self.assertIn("expired", str(caught.exception))

    def test_unexpired_oauth_returns_access_token(self) -> None:
        self.store.set_oauth("copilot", "token-value-long", expires=4_102_444_800)
        self.assertEqual(self.store.secret("copilot"), "token-value-long")
        self.assertFalse(self.store.is_expired("copilot"))

    def test_malformed_store_is_reported_not_silently_ignored(self) -> None:
        self.store.path.parent.mkdir(parents=True, exist_ok=True)
        self.store.path.write_text('{"credentials": "not-an-object"}', encoding="utf-8")
        with self.assertRaises(CredentialError):
            self.store.names()


class ResolveSecretTests(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.root = Path(self._dir.name)

    def tearDown(self) -> None:
        self._dir.cleanup()
        os.environ.pop("ODIN_TEST_KEY", None)
        os.environ.pop("ODIN_CREDENTIAL_OPENROUTER", None)

    def test_environment_wins_over_store(self) -> None:
        CredentialStore(self.root).set_api_key("openrouter", "stored-value-long")
        os.environ["ODIN_TEST_KEY"] = "env-value-long"
        self.assertEqual(
            resolve_secret(self.root, "openrouter", "ODIN_TEST_KEY"), "env-value-long"
        )

    def test_falls_back_to_store_when_env_unset(self) -> None:
        CredentialStore(self.root).set_api_key("openrouter", "stored-value-long")
        self.assertEqual(
            resolve_secret(self.root, "openrouter", "ODIN_TEST_KEY"), "stored-value-long"
        )

    def test_per_credential_env_override(self) -> None:
        CredentialStore(self.root).set_api_key("openrouter", "stored-value-long")
        os.environ["ODIN_CREDENTIAL_OPENROUTER"] = "injected-value-long"
        self.assertEqual(
            resolve_secret(self.root, "openrouter", None), "injected-value-long"
        )

    def test_no_credential_configured_is_not_an_error(self) -> None:
        # Local servers legitimately need no key.
        self.assertIsNone(resolve_secret(self.root, None, None))

    def test_declared_env_var_missing_is_an_error(self) -> None:
        with self.assertRaises(CredentialError):
            resolve_secret(self.root, None, "ODIN_TEST_KEY")


if __name__ == "__main__":
    unittest.main()
