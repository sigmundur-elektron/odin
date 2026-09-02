from __future__ import annotations

import argparse
import json
import os
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str], cwd: Path, environment: dict[str, str]) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60,
    )
    if completed.returncode != 0:
        raise AssertionError(
            f"command failed ({completed.returncode}): {command!r}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed


def quoted(value: str | Path) -> str:
    return json.dumps(str(value))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--config", default="")
    parser.add_argument("--bindir", required=True)
    parser.add_argument("--runtime-dir", required=True)
    # the deterministic child from tests/cpp/test_child.cpp, standing in for a
    # project-supplied gate. a consuming project may still use any language;
    # Odin's own smoke test must not need one.
    parser.add_argument("--helper", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="odin install test ") as temporary:
        root = Path(temporary)
        # spaces in every path on purpose: quoting bugs only show up here
        prefix = root / "install prefix"
        project = root / "consuming project"
        invocation = root / "unrelated invocation"
        project.mkdir()
        invocation.mkdir()

        install = [args.cmake, "--install", args.build_dir, "--prefix", str(prefix)]
        if args.config:
            install.extend(["--config", args.config])
        run(install, invocation, dict(os.environ))

        executable = prefix / args.bindir / ("odin.exe" if os.name == "nt" else "odin")
        runtime = prefix / args.runtime_dir
        required = [
            executable,
            runtime / "harness" / "schemas" / "task.schema.json",
            runtime / "harness" / "workflows" / "feature.json",
            runtime / "harness" / "agents" / "implementer.json",
        ]
        missing = [str(path) for path in required if not path.exists()]
        if missing:
            raise AssertionError("installed runtime is incomplete: " + ", ".join(missing))

        # the install tree must carry no interpreter and no scripts
        stray = sorted(
            str(path.relative_to(prefix))
            for path in prefix.rglob("*")
            if path.suffix in {".py", ".pyc"} or path.name == "requirements.txt"
        )
        if stray:
            raise AssertionError("install prefix contains Python: " + ", ".join(stray))

        (project / "project.txt").write_text("installed layout sentinel\n", encoding="utf-8")
        (project / "task.json").write_text(
            json.dumps(
                {
                    "id": "installed-smoke",
                    "kind": "feature",
                    "title": "Installed layout smoke test",
                    "request": "Complete the deterministic installed-layout workflow.",
                    "context": {},
                    "constraints": [],
                }
            ),
            encoding="utf-8",
        )

        config = f"""[harness]
state_dir = ".odin/runs"
max_total_transitions = 30

[git]
stage_on_success = false

[adapters.smoke]
type = "mock"

[models.smoke]
adapter = "smoke"
model = "installed-layout"
tags = ["test"]

[routing]
default = "smoke"

[gates.quality]
command = [{quoted(args.helper)}, "out:installed project gate passed"]
timeout_seconds = 30
"""
        config_path = project / "odin.toml"
        config_path.write_text(config, encoding="utf-8")

        environment = dict(os.environ)
        environment.pop("PYTHONPATH", None)
        environment.pop("ODIN_RUNTIME_ROOT", None)

        validated = run([str(executable), "validate", "--self-only"], invocation, environment)
        if json.loads(validated.stdout)["status"] != "valid":
            raise AssertionError("installed definitions did not validate")

        run([str(executable), "--config", str(config_path), "tools", "list"], invocation, environment)
        started = run(
            [str(executable), "--config", str(config_path), "start", str(project / "task.json")],
            invocation,
            environment,
        )
        payload = json.loads(started.stdout)
        if payload["state"]["status"] != "complete":
            raise AssertionError(f"installed run did not complete: {payload!r}")

        run_dir = Path(payload["run_dir"])
        state = json.loads((run_dir / "state.json").read_text(encoding="utf-8"))
        context = json.loads((run_dir / "context.json").read_text(encoding="utf-8"))
        if state["schema_version"] != 2 or state["status"] != "complete" or len(context["history"]) != 8:
            raise AssertionError("installed run state is incomplete")
        journal = [
            json.loads(path.read_text(encoding="utf-8"))
            for path in sorted((run_dir / "journal").glob("*.json"))
        ]
        if len(journal) != 16:
            raise AssertionError("installed run did not journal every stage attempt")
        started_ids = {
            record["execution_id"] for record in journal if record["type"] == "stage_started"
        }
        completed_ids = {
            record["execution_id"] for record in journal if record["type"] == "stage_completed"
        }
        if started_ids != completed_ids:
            raise AssertionError("installed run has an unmatched stage attempt")
        gate_records = [record for record in context["history"] if record["kind"] == "gate"]
        if "installed project gate passed" not in gate_records[0]["result"]["artifacts"]["gate"]["output"]:
            raise AssertionError("project-relative gate did not execute")
        if (prefix / ".odin").exists():
            raise AssertionError("mutable project state was written into the install prefix")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
