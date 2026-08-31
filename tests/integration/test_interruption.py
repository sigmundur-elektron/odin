from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import tempfile
import time
from pathlib import Path


def run(command: list[str], cwd: Path, environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )


def wait_for(predicate, description: str, timeout: float = 20.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {description}")


def process_exists(pid: int) -> bool:
    try:
        os.kill(pid, 0)
    except (OSError, ProcessLookupError):
        return False
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--odin", required=True)
    parser.add_argument("--runtime-root", required=True)
    parser.add_argument("--python", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="odin interruption test ") as temporary:
        project = Path(temporary)
        task = project / "task.json"
        task.write_text(
            json.dumps(
                {
                    "id": "interrupt-smoke",
                    "kind": "feature",
                    "title": "Interruption smoke",
                    "request": "Exercise interruption recovery.",
                }
            ),
            encoding="utf-8",
        )
        agent = project / "agent.py"
        agent.write_text(
            """import json, os, sys, time
from pathlib import Path
counter = Path('invocations.txt')
count = int(counter.read_text() or '0') + 1 if counter.exists() else 1
counter.write_text(str(count))
Path('agent.pid').write_text(str(os.getpid()))
if count == 1:
    Path('first-started').write_text('ready')
    time.sleep(120)
json.dump({'status': 'approved', 'summary': 'approved',
           'artifacts': {'changed_files': ['task.json']}, 'findings': []}, sys.stdout)
""",
            encoding="utf-8",
        )
        config = project / "odin.toml"
        config.write_text(
            f"""[harness]
state_dir = ".odin/runs"
max_total_transitions = 30
[git]
stage_on_success = false
[adapters.test]
command = [{json.dumps(args.python)}, {json.dumps(str(agent))}, "--model", "{{model}}"]
timeout_seconds = 180
[models.test]
adapter = "test"
model = "interrupt"
[routing]
default = "test"
[gates.quality]
command = [{json.dumps(args.python)}, "-c", "raise SystemExit(0)"]
timeout_seconds = 30
""",
            encoding="utf-8",
        )

        environment = dict(os.environ)
        environment["ODIN_RUNTIME_ROOT"] = args.runtime_root
        environment["ODIN_PYTHON"] = args.python
        start = subprocess.Popen(
            [args.odin, "--config", str(config), "start", str(task)],
            cwd=project,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        try:
            wait_for(lambda: (project / "first-started").exists(), "the first adapter invocation")
            run_dir = wait_for(
                lambda: next((path for path in (project / ".odin" / "runs").glob("*") if path.is_dir()), None),
                "the run directory",
            )
            state = json.loads((run_dir / "state.json").read_text(encoding="utf-8"))
            if state.get("in_progress", {}).get("stage") != "specify":
                raise AssertionError("the active attempt was not persisted before the adapter ran")

            concurrent = run(
                [args.odin, "--config", str(config), "resume", str(run_dir)],
                project,
                environment,
            )
            if concurrent.returncode != 2 or "already being executed" not in concurrent.stderr:
                raise AssertionError(f"concurrent resume was not rejected: {concurrent!r}")

            start.kill()
            start.wait(timeout=10)
            agent_pid = int((project / "agent.pid").read_text())
            try:
                os.kill(agent_pid, signal.SIGTERM)
            except (OSError, ProcessLookupError):
                pass
            else:
                wait_for(
                    lambda: not process_exists(agent_pid),
                    "the interrupted adapter process to stop",
                    timeout=10,
                )

            plain = run(
                [args.odin, "--config", str(config), "resume", str(run_dir)],
                project,
                environment,
            )
            if plain.returncode != 2:
                raise AssertionError(f"plain interrupted resume unexpectedly succeeded: {plain!r}")
            blocked = json.loads(plain.stdout)["state"]
            if blocked.get("reason_code") != "outcome_uncertain":
                raise AssertionError(f"interrupted run was not marked uncertain: {blocked!r}")
            if (project / "invocations.txt").read_text() != "1":
                raise AssertionError("plain resume reran uncertain external work")

            retried = run(
                [
                    args.odin,
                    "--config",
                    str(config),
                    "resume",
                    str(run_dir),
                    "--retry-interrupted",
                ],
                project,
                environment,
            )
            if retried.returncode != 0 or json.loads(retried.stdout)["state"]["status"] != "complete":
                raise AssertionError(f"explicit retry did not complete: {retried!r}")
            if int((project / "invocations.txt").read_text()) <= 1:
                raise AssertionError("explicit retry did not execute a new attempt")
        finally:
            if start.poll() is None:
                start.kill()
                start.wait(timeout=10)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
