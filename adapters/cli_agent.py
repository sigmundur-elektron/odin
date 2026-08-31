#!/usr/bin/env python3
"""Universal agent-CLI adapter.

Wraps any command-line coding agent that accepts a prompt and prints a result:
OpenCode, Claude Code, Aider, Codex, llm, or a shell script of your own. The
agent command is supplied entirely on the command line, so Odin gains providers
without gaining dependencies.

    python adapters/cli_agent.py --model M -- opencode run --model M
    python adapters/cli_agent.py --model M --jsonl -- some-agent --stream

The prompt is appended as the final argument by default, or piped on stdin with
--prompt-stdin. Reads one Odin agent request on stdin, writes one handoff object
on stdout.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from harness.credentials import CredentialError, resolve_secret  # noqa: E402
from harness.extract import (  # noqa: E402
    ExtractionError,
    concat_event_text,
    extract_json_object,
)

HANDOFF_SHAPE = (
    '{"status":"approved|revision|blocked","summary":"one sentence",'
    '"artifacts":{},"findings":[]}'
)


def build_prompt(request: dict, max_context_chars: int) -> str:
    agent = request.get("agent", {})
    skills = request.get("skills", [])
    task = request.get("task", {})
    artifacts = request.get("artifacts", {})

    lines = [
        f"You are the '{agent.get('id', 'agent')}' stage of an automated software workflow.",
        f"Purpose: {agent.get('purpose', '')}",
    ]
    if agent.get("rules"):
        lines.append("Rules:")
        lines.extend(f"- {rule}" for rule in agent["rules"])
    for skill in skills:
        lines.append(f"Skill '{skill.get('id')}': {json.dumps(skill, separators=(',', ':'))}")

    artifacts_text = json.dumps(artifacts, indent=2, sort_keys=True)
    if len(artifacts_text) > max_context_chars:
        artifacts_text = artifacts_text[:max_context_chars] + "\n... truncated ..."

    lines.extend(
        [
            "",
            "Task:",
            json.dumps(task, indent=2, sort_keys=True),
            "",
            "Artifacts from previous stages:",
            artifacts_text,
            "",
            "Respond with exactly one JSON object and nothing else, no markdown "
            "fences and no commentary. Required shape: " + HANDOFF_SHAPE,
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--max-context-chars", type=int, default=24000)
    parser.add_argument(
        "--jsonl",
        action="store_true",
        help="treat stdout as newline-delimited events and concatenate text parts",
    )
    parser.add_argument(
        "--text-path",
        default="part.text",
        help="dotted field path to assistant text within each event (with --jsonl)",
    )
    parser.add_argument(
        "--prompt-stdin",
        action="store_true",
        help="pipe the prompt on stdin instead of appending it as the last argument",
    )
    parser.add_argument(
        "--credential",
        default=None,
        help="stored credential to expose to the agent process",
    )
    parser.add_argument(
        "--credential-env",
        default=None,
        help="environment variable name the agent expects the secret in "
             "(used with --credential; the value is passed via the child's "
             "environment, never on its command line)",
    )
    parser.add_argument(
        "--project-root",
        default=None,
        help="repository root containing .odin/credentials.json",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="the agent command, after a literal --",
    )
    args = parser.parse_args()

    command = [part for part in args.command if part != "--"]
    if not command:
        print("no agent command supplied after --", file=sys.stderr)
        return 1

    try:
        request = json.load(sys.stdin)
    except json.JSONDecodeError as error:
        print(f"invalid request on stdin: {error}", file=sys.stderr)
        return 1

    prompt = build_prompt(request, args.max_context_chars)
    command = [part.replace("{model}", args.model) for part in command]
    stdin_text = prompt if args.prompt_stdin else None
    if not args.prompt_stdin:
        command.append(prompt)

    environment = dict(os.environ)
    if args.credential:
        root = Path(
            args.project_root
            or os.environ.get("ODIN_PROJECT_ROOT")
            or Path.cwd()
        )
        try:
            secret = resolve_secret(root, args.credential, None)
        except CredentialError as error:
            print(str(error), file=sys.stderr)
            return 1
        if secret and args.credential_env:
            environment[args.credential_env] = secret

    try:
        completed = subprocess.run(
            command,
            input=stdin_text,
            env=environment,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=args.timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"agent command failed: {error}", file=sys.stderr)
        return 1

    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()[:400]
        print(f"agent exited {completed.returncode}: {detail}", file=sys.stderr)
        return 1

    raw = completed.stdout
    text = concat_event_text(raw, args.text_path) if args.jsonl else raw
    if args.jsonl and not text.strip():
        text = raw

    try:
        handoff = extract_json_object(text)
    except ExtractionError as error:
        print(f"{args.model} did not return a JSON object: {error}", file=sys.stderr)
        return 1

    json.dump(handoff, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
