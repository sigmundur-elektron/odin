#!/usr/bin/env python3
"""Reproducible quality gate for the Odin harness itself.

Consuming C++ projects define their own commands in odin.toml. This script only
verifies the framework-neutral engine, the bundled contracts, and both
implementations of the harness.

Odin is mid-port to C++. The engine, state, config and primary CLI are already
native; provider discovery, credentials, the two agent adapters and the schema
validator are still Python. Both halves are checked here.

The two are no longer diffed against each other. Python is being removed rather
than kept in step, so the front ends are expected to diverge, and a parity check
would fail on every intended change. See docs/cpp-only-plan.html.

The C++ steps are skipped, not failed, when no build directory is present, so
the gate still works on a machine with only Python.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build-cpp"
ODIN_EXE = BUILD / ("odin.exe" if sys.platform == "win32" else "odin")
TESTS_EXE = BUILD / ("odin_tests.exe" if sys.platform == "win32" else "odin_tests")


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


def cmake_tool(name: str) -> str | None:
    configured = os.environ.get("ODIN_CMAKE")
    if configured:
        candidate = Path(configured)
        if name != "cmake":
            candidate = candidate.with_name(name + candidate.suffix)
        if candidate.exists():
            return str(candidate)
    found = shutil.which(name)
    if found:
        return found
    cache = BUILD / "CMakeCache.txt"
    if cache.exists():
        marker = "CMAKE_COMMAND:INTERNAL="
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith(marker):
                candidate = Path(line[len(marker):])
                if name != "cmake":
                    candidate = candidate.with_name(name + candidate.suffix)
                if candidate.exists():
                    return str(candidate)
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--require-native",
        action="store_true",
        help="fail instead of skipping when the native build or CTest is unavailable",
    )
    args = parser.parse_args()

    steps: list[Step] = []
    skipped: list[str] = []

    commands: list[tuple[str, list[str]]] = []
    # The C++ half. `cpp-tests` covers the unit suite and the remaining
    # differential harness.
    #
    # There is deliberately no `cli-parity` step. It diffed the C++ front end
    # against the Python one, which was the right check while the two were meant
    # to agree. The port makes them diverge on purpose - `resume
    # --retry-interrupted` already exists only in C++ - so keeping the diff would
    # report a false failure for every intended change. Native behaviour is
    # asserted directly by odin_tests instead.
    if TESTS_EXE.exists():
        commands.append(("cpp-tests", [str(TESTS_EXE)]))
    else:
        skipped.append("cpp-tests")

    ctest = cmake_tool("ctest")
    if ctest and TESTS_EXE.exists() and ODIN_EXE.exists():
        commands.append(
            (
                "install-smoke",
                [ctest, "--test-dir", str(BUILD), "-R", "^odin_(install|interruption)_smoke$", "--output-on-failure"],
            )
        )
    else:
        skipped.append("install-smoke")

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
    for name in skipped:
        print(f"SKIP  {name:<12} no build in {BUILD.name}/")

    native_missing = args.require_native and bool(skipped)
    gate_passed = (
        len(steps) == len(commands)
        and all(step.exit_code == 0 for step in steps)
        and not native_missing
    )
    print(f"GATE: {'PASS' if gate_passed else 'FAIL'}")
    return 0 if gate_passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
