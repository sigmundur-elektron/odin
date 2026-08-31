"""Diff the C++ CLI against the Python CLI, command by command.

The unit suite already proves the two engines write identical durable state
(tests/cpp/test_engine_parity.cpp). This covers the layer above it: argument
parsing, exit codes, and what each command prints.

    python scripts/compare_cli.py [path-to-odin.exe]

JSON output is compared as parsed values rather than bytes, because
harness/cli.py prints with sort_keys=False while the port sorts. Key order
carries no meaning in JSON and the state files themselves are sorted; that
divergence is documented in src/cli.cpp. Plain-text output is compared verbatim.
"""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# (argv, "json" | "text") - argv is appended to each front-end's own prefix
CASES: list[tuple[list[str], str]] = [
    (["validate", "--self-only"], "json"),
    (["validate"], "json"),
    (["models"], "text"),
    (["models", "--json"], "json"),
    (["auth", "list"], "text"),
    (["tools", "list"], "text"),
    (["--config", "does-not-exist.toml", "models"], "text"),
    (["start", "no-such-template"], "text"),
    (["start", "feature", "--set", "bad-override"], "text"),
]


def run(command: list[str], cwd: Path) -> tuple[int, str, str]:
    done = subprocess.run(
        command, cwd=cwd, capture_output=True, text=True,
        encoding="utf-8", errors="replace",
    )
    return done.returncode, done.stdout, done.stderr


def compare(name: str, mode: str, py: tuple[int, str, str], cc: tuple[int, str, str]) -> bool:
    problems = []

    if py[0] != cc[0]:
        problems.append(f"exit code: python={py[0]} c++={cc[0]}")

    if mode == "json" and py[1].strip() and cc[1].strip():
        try:
            if json.loads(py[1]) != json.loads(cc[1]):
                problems.append("stdout differs semantically")
        except json.JSONDecodeError as error:
            problems.append(f"stdout is not valid json on both sides: {error}")
    elif py[1] != cc[1]:
        problems.append("stdout differs")

    if problems:
        print(f"FAIL  {name}")
        for problem in problems:
            print(f"        {problem}")
        if "stdout differs" in " ".join(problems):
            print(f"        python: {py[1]!r}")
            print(f"        c++   : {cc[1]!r}")
        return False

    print(f"ok    {name}")
    return True


def main() -> int:
    odin_exe = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build-cpp" / "odin.exe"
    if not odin_exe.exists():
        print(f"not found: {odin_exe}", file=sys.stderr)
        return 2

    failures = 0
    for argv, mode in CASES:
        name = " ".join(argv)
        with tempfile.TemporaryDirectory():
            py = run([sys.executable, str(ROOT / "odin.py"), *argv], ROOT)
            cc = run([str(odin_exe), *argv], ROOT)
        if not compare(name, mode, py, cc):
            failures += 1

    print()
    print(f"{len(CASES) - failures}/{len(CASES)} commands agree")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
