#!/usr/bin/env python3
"""Reproducible quality gate for the Odin harness itself.

Consuming C++ projects define their own commands in odin.toml. This script only
verifies the framework-neutral engine, bundled contracts, and Python sources.
"""

from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


@dataclass(frozen=True)
class Step:
    name: str
    command: list[str]
    exit_code: int
    output: str


def run_step(name: str, command: list[str]) -> Step:
    completed = subprocess.run(
        command,
        cwd=ROOT,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return Step(name, command, completed.returncode, completed.stdout)


def main() -> int:
    steps: list[Step] = []
    commands = [
        ("contracts", [sys.executable, "odin.py", "validate", "--self-only"]),
        ("tests", [sys.executable, "-m", "unittest", "discover", "-s", "tests", "-v"]),
        ("compile", [sys.executable, "-m", "compileall", "-q", "harness", "odin.py", "scripts/mock_agent.py"]),
    ]
    for name, command in commands:
        print(f"=== {name} ===")
        print("$ " + " ".join(command))
        step = run_step(name, command)
        steps.append(step)
        print(step.output, end="" if step.output.endswith("\n") or not step.output else "\n")
        if step.exit_code != 0:
            break

    print("\n=== GATE SUMMARY ===")
    for step in steps:
        verdict = "PASS" if step.exit_code == 0 else "FAIL"
        print(f"{verdict:<5} {step.name:<12} exit {step.exit_code}")
    gate_passed = len(steps) == len(commands) and all(step.exit_code == 0 for step in steps)
    print(f"GATE: {'PASS' if gate_passed else 'FAIL'}")
    return 0 if gate_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
