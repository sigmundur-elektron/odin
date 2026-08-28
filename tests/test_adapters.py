from __future__ import annotations

import json
import subprocess
import sys
import threading
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

REQUEST = {
    "agent": {
        "id": "verifier",
        "purpose": "Map gate evidence to acceptance criteria.",
        "rules": ["Do not edit files."],
    },
    "skills": [{"id": "verification", "procedure": ["read gate output"]}],
    "stage": {"id": "verify", "kind": "agent"},
    "task": {"id": "t1", "kind": "feature", "title": "T", "request": "R"},
    "artifacts": {"gate_result": {"status": "passed"}},
}

HANDOFF = {"status": "approved", "summary": "verified", "artifacts": {}, "findings": []}


class _Handler(BaseHTTPRequestHandler):
    content = json.dumps(HANDOFF)
    captured: dict = {}

    def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
        length = int(self.headers.get("Content-Length", "0"))
        _Handler.captured = json.loads(self.rfile.read(length) or b"{}")
        body = json.dumps(
            {"choices": [{"message": {"role": "assistant", "content": self.content}}]}
        ).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args) -> None:
        return


class OpenAiCompatibleAdapterTests(unittest.TestCase):
    def setUp(self) -> None:
        _Handler.content = json.dumps(HANDOFF)
        self.server = HTTPServer(("127.0.0.1", 0), _Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.base_url = f"http://127.0.0.1:{self.server.server_address[1]}/v1"

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def _run(self) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(ROOT / "adapters" / "openai_compatible.py"),
                "--model", "test-model",
                "--base-url", self.base_url,
            ],
            input=json.dumps(REQUEST),
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_returns_valid_handoff(self) -> None:
        completed = self._run()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout), HANDOFF)

    def test_sends_agent_identity_and_model(self) -> None:
        self._run()
        payload = _Handler.captured
        self.assertEqual(payload["model"], "test-model")
        system = payload["messages"][0]["content"]
        self.assertIn("verifier", system)
        self.assertIn("Do not edit files.", system)

    def test_recovers_handoff_wrapped_in_markdown(self) -> None:
        _Handler.content = f"Sure thing:\n```json\n{json.dumps(HANDOFF)}\n```"
        completed = self._run()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout), HANDOFF)

    def test_non_json_reply_fails_loudly(self) -> None:
        _Handler.content = "I am unable to comply."
        completed = self._run()
        self.assertEqual(completed.returncode, 1)
        self.assertIn("did not return a JSON object", completed.stderr)

    def test_unreachable_endpoint_reports_clearly(self) -> None:
        completed = subprocess.run(
            [
                sys.executable,
                str(ROOT / "adapters" / "openai_compatible.py"),
                "--model", "m",
                "--base-url", "http://127.0.0.1:1/v1",
            ],
            input=json.dumps(REQUEST),
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn("unreachable", completed.stderr)


class CliAgentAdapterTests(unittest.TestCase):
    def _run(self, agent_source: str, extra: list[str] | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [
                sys.executable,
                str(ROOT / "adapters" / "cli_agent.py"),
                "--model", "test-model",
                *(extra or []),
                "--", sys.executable, "-c", agent_source,
            ],
            input=json.dumps(REQUEST),
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_plain_stdout_agent(self) -> None:
        source = f"import sys; print({json.dumps(json.dumps(HANDOFF))})"
        completed = self._run(source)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout), HANDOFF)

    def test_streaming_jsonl_agent(self) -> None:
        events = [
            {"type": "text", "part": {"text": json.dumps(HANDOFF)[:20]}},
            {"type": "text", "part": {"text": json.dumps(HANDOFF)[20:]}},
        ]
        printed = "\n".join(json.dumps(event) for event in events)
        source = f"print({json.dumps(printed)})"
        completed = self._run(source, ["--jsonl"])
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout), HANDOFF)

    def test_failing_agent_propagates_error(self) -> None:
        completed = self._run("import sys; sys.stderr.write('boom'); sys.exit(3)")
        self.assertEqual(completed.returncode, 1)
        self.assertIn("agent exited 3", completed.stderr)

    def test_missing_command_is_rejected(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(ROOT / "adapters" / "cli_agent.py"), "--model", "m"],
            input=json.dumps(REQUEST),
            capture_output=True,
            text=True,
            timeout=60,
        )
        self.assertEqual(completed.returncode, 1)
        self.assertIn("no agent command", completed.stderr)


if __name__ == "__main__":
    unittest.main()
