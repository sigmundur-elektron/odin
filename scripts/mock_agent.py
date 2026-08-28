#!/usr/bin/env python3
"""Deterministic adapter used for harness development and contract tests.

Reads one Odin agent request from stdin and writes one handoff object to stdout.
It is not an AI model and must not be used as implementation evidence.
"""

from __future__ import annotations

import argparse
import json
import sys


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default="mock")
    args = parser.parse_args()
    request = json.load(sys.stdin)
    agent = request["agent"]["id"]
    artifacts = {}
    if agent == "analyst":
        artifacts = {
            "requirements": [request["task"]["request"]],
            "acceptance_criteria": ["Configured quality gate exits 0."],
            "non_goals": [],
            "changed_files": ["README.md"],
        }
    elif agent == "reproducer":
        artifacts = {"reproduced": True, "command": ["mock"], "exit_code": 0}
    elif agent == "implementer":
        artifacts = {"changed_files": ["README.md"], "notes": ["mock implementation"]}
    elif agent == "verifier":
        artifacts = {"criteria": [{"id": "A1", "status": "passed"}], "gaps": []}
    elif agent == "finalizer":
        artifacts = {"summary": "mock workflow complete", "changed_files": ["README.md"]}
    else:
        artifacts = {"findings": []}
    json.dump(
        {
            "status": "approved",
            "summary": f"{agent} approved using {args.model}",
            "artifacts": artifacts,
            "findings": [],
        },
        sys.stdout,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
