#!/usr/bin/env python3
"""Universal OpenAI-compatible adapter.

One adapter covers Ollama, LM Studio, llama.cpp, vLLM, LiteLLM, OpenRouter,
OpenAI, Azure, Groq, Together, DeepSeek and anything else exposing
POST /chat/completions. The provider is selected entirely by --base-url and
--api-key-env, so adding a provider is configuration, not code.

Reads one Odin agent request on stdin, writes one handoff object on stdout.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from harness.credentials import CredentialError, resolve_secret  # noqa: E402
from harness.extract import ExtractionError, extract_json_object  # noqa: E402

HANDOFF_SHAPE = (
    '{"status": "approved" | "revision" | "blocked", '
    '"summary": "one sentence", '
    '"artifacts": {}, '
    '"findings": ["blocking issue", "..."]}'
)


def build_messages(request: dict, max_context_chars: int) -> list[dict[str, str]]:
    agent = request.get("agent", {})
    skills = request.get("skills", [])
    stage = request.get("stage", {})
    task = request.get("task", {})
    artifacts = request.get("artifacts", {})

    system = [
        f"You are the '{agent.get('id', 'agent')}' stage of an automated software workflow.",
        f"Purpose: {agent.get('purpose', '')}",
    ]
    if agent.get("rules"):
        system.append("Rules:")
        system.extend(f"- {rule}" for rule in agent["rules"])
    for skill in skills:
        system.append(f"Skill '{skill.get('id')}': " + json.dumps(skill, separators=(",", ":")))
    system.append(
        "Reply with exactly one JSON object and nothing else. No markdown fences, "
        "no commentary before or after. Required shape: " + HANDOFF_SHAPE
    )
    system.append(
        "Use status 'approved' when your stage succeeded, 'revision' when the previous "
        "stage must be redone, and 'blocked' when you cannot proceed. Put concrete "
        "blocking problems in findings."
    )

    artifacts_text = json.dumps(artifacts, indent=2, sort_keys=True)
    if len(artifacts_text) > max_context_chars:
        artifacts_text = (
            artifacts_text[:max_context_chars]
            + f"\n... truncated at {max_context_chars} characters ..."
        )

    user = "\n".join(
        [
            "Stage: " + json.dumps(stage, separators=(",", ":")),
            "",
            "Task:",
            json.dumps(task, indent=2, sort_keys=True),
            "",
            "Artifacts from previous stages:",
            artifacts_text,
        ]
    )
    return [
        {"role": "system", "content": "\n".join(system)},
        {"role": "user", "content": user},
    ]


def call(base_url: str, api_key: str | None, payload: dict, timeout: int) -> dict:
    request = urllib.request.Request(
        f"{base_url.rstrip('/')}/chat/completions",
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    if api_key:
        request.add_header("Authorization", f"Bearer {api_key}")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8", errors="replace"))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument(
        "--api-key-env",
        default=None,
        help="environment variable holding the key; checked before the credential store",
    )
    parser.add_argument(
        "--credential",
        default=None,
        help="name of a credential stored via `odin auth set`; read here so the "
             "secret never appears in the process command line",
    )
    parser.add_argument(
        "--project-root",
        default=None,
        help="repository root containing .odin/credentials.json (default: Odin project root)",
    )
    parser.add_argument("--temperature", type=float, default=0.0)
    parser.add_argument("--timeout", type=int, default=900)
    parser.add_argument("--max-context-chars", type=int, default=24000)
    parser.add_argument(
        "--no-json-mode",
        action="store_true",
        help="omit response_format; needed by servers that reject the field",
    )
    args = parser.parse_args()

    try:
        request = json.load(sys.stdin)
    except json.JSONDecodeError as error:
        print(f"invalid request on stdin: {error}", file=sys.stderr)
        return 1

    payload: dict = {
        "model": args.model,
        "messages": build_messages(request, args.max_context_chars),
        "temperature": args.temperature,
    }
    if not args.no_json_mode:
        payload["response_format"] = {"type": "json_object"}

    api_key = None
    if args.api_key_env or args.credential:
        root = Path(
            args.project_root
            or os.environ.get("ODIN_PROJECT_ROOT")
            or Path.cwd()
        )
        try:
            api_key = resolve_secret(root, args.credential, args.api_key_env)
        except CredentialError as error:
            print(str(error), file=sys.stderr)
            return 1

    try:
        response = call(args.base_url, api_key, payload, args.timeout)
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8", errors="replace")[:400]
        # Retry once without json mode: several local servers 400 on it.
        if error.code == 400 and not args.no_json_mode:
            payload.pop("response_format", None)
            try:
                response = call(args.base_url, api_key, payload, args.timeout)
            except (urllib.error.URLError, OSError) as retry_error:
                print(f"provider error after retry: {retry_error}", file=sys.stderr)
                return 1
        else:
            print(f"provider HTTP {error.code}: {body}", file=sys.stderr)
            return 1
    except (urllib.error.URLError, OSError) as error:
        print(f"provider unreachable at {args.base_url}: {error}", file=sys.stderr)
        return 1

    try:
        content = response["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        print(f"unexpected provider response: {json.dumps(response)[:400]}", file=sys.stderr)
        return 1

    try:
        handoff = extract_json_object(content or "")
    except ExtractionError as error:
        print(f"{args.model} did not return a JSON object: {error}", file=sys.stderr)
        return 1

    json.dump(handoff, sys.stdout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
