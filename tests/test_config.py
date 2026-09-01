from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from harness.config import load_config
from harness.errors import WorkflowError


VALID = """
[harness]
state_dir = ".odin/runs"
[adapters.mock]
command = ["python", "mock.py", "{model}"]
[models.small]
adapter = "mock"
model = "coder-32b"
parameter_billions = 32
[routing]
default = "small"
[gates.quality]
command = ["python", "-m", "unittest"]
"""


class ConfigTests(unittest.TestCase):
    def test_model_size_is_metadata_not_a_hardcoded_threshold(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "odin.toml"
            path.write_text(VALID, encoding="utf-8")
            config = load_config(path)
            self.assertEqual(config.models["small"].parameter_billions, 32)
            self.assertEqual(config.model_for("any-agent").model, "coder-32b")

    def test_route_to_unknown_model_fails(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "odin.toml"
            path.write_text(VALID.replace('default = "small"', 'default = "missing"'), encoding="utf-8")
            config = load_config(path)
            with self.assertRaises(WorkflowError):
                config.model_for("agent")

    def test_command_environment_and_git_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "odin.toml"
            path.write_text(
                VALID.replace(
                    'command = ["python", "mock.py", "{model}"]',
                    'command = ["python", "mock.py", "{model}"]\n'
                    'inherit_environment = ["OPENAI_API_KEY"]\n'
                    'environment = { PYTHONUTF8 = "1" }',
                ) + "\n[git]\ntimeout_seconds = 17\n",
                encoding="utf-8",
            )
            config = load_config(path)
            self.assertEqual(config.adapters["mock"].inherit_environment, ("OPENAI_API_KEY",))
            self.assertEqual(config.adapters["mock"].environment, {"PYTHONUTF8": "1"})
            self.assertEqual(config.git_timeout_seconds, 17)

    def test_invalid_git_shape_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "odin.toml"
            path.write_text("git = 3\n", encoding="utf-8")
            with self.assertRaisesRegex(WorkflowError, "git must be a table"):
                load_config(path)


if __name__ == "__main__":
    unittest.main()
