from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from harness.config import load_config
from harness.engine import WorkflowEngine
from harness.io import read_json


ROOT = Path(__file__).resolve().parent.parent


class EngineTests(unittest.TestCase):
    def _config(self, directory: Path, gate_exit: int = 0):
        config_path = directory / "odin.toml"
        mock = ROOT / "scripts" / "mock_agent.py"
        config_path.write_text(
            f"""
[harness]
state_dir = ".odin/runs"
max_total_transitions = 30
[git]
stage_on_success = false
[adapters.mock]
command = ["python", "{mock.as_posix()}", "--model", "{{model}}"]
[models.test]
adapter = "mock"
model = "fixture"
parameter_billions = 0
[routing]
default = "test"
[gates.quality]
command = ["python", "-c", "raise SystemExit({gate_exit})"]
""",
            encoding="utf-8",
        )
        return load_config(config_path)

    def test_feature_completes_and_keeps_event_log(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self._config(root)
            engine = WorkflowEngine(config)
            task = {"id": "feature-1", "kind": "feature", "title": "Feature", "request": "Add it."}
            run_dir = engine.create_run(task, root / "feature.json")
            state = engine.run(run_dir)
            self.assertEqual(state["status"], "complete")
            context = read_json(run_dir / "context.json")
            self.assertEqual(
                [event["stage"] for event in context["history"]],
                ["specify", "review-spec", "spec-checkpoint", "implement", "quality-gate", "verify", "finalize", "stage"],
            )
            self.assertEqual(context["artifacts"]["staging"]["artifacts"]["staging_manifest"], ["README.md"])

    def test_bugfix_reproduces_before_specification(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self._config(root)
            engine = WorkflowEngine(config)
            task = {
                "id": "bug-1",
                "kind": "bugfix",
                "title": "Bug",
                "request": "Fix it.",
                "reproduction": {"steps": ["run it"], "expected": "works", "actual": "fails"},
            }
            run_dir = engine.create_run(task, root / "bug.json")
            state = engine.run(run_dir)
            self.assertEqual(state["status"], "complete")
            context = read_json(run_dir / "context.json")
            self.assertEqual(context["history"][0]["stage"], "reproduce")
            self.assertIn("regression-checkpoint", [event["stage"] for event in context["history"]])

    def test_failing_gate_loops_back_then_blocks_at_attempt_limit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self._config(root, gate_exit=1)
            engine = WorkflowEngine(config)
            task = {"id": "feature-1", "kind": "feature", "title": "Feature", "request": "Add it."}
            run_dir = engine.create_run(task, root / "feature.json")
            state = engine.run(run_dir)
            self.assertEqual(state["status"], "blocked")
            self.assertIn("attempt limit", state["reason"])
            context = read_json(run_dir / "context.json")
            stages = [event["stage"] for event in context["history"]]
            self.assertGreater(stages.count("implement"), 1)
            self.assertGreater(stages.count("quality-gate"), 1)

    def test_automatic_staging_is_disabled_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            config = self._config(root)
            engine = WorkflowEngine(config)
            task = {"id": "feature-1", "kind": "feature", "title": "Feature", "request": "Add it."}
            run_dir = engine.create_run(task, root / "feature.json")
            engine.run(run_dir)
            context = read_json(run_dir / "context.json")
            staging = context["artifacts"]["staging"]["artifacts"]
            self.assertEqual(staging["staged_files"], [])
            self.assertEqual(staging["staging_manifest"], ["README.md"])


if __name__ == "__main__":
    unittest.main()
