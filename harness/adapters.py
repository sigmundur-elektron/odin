from __future__ import annotations

import json
import os
import subprocess
import time
from typing import Any

from .config import CommandSpec, ModelProfile, ProjectConfig
from .errors import AdapterError


def run_command_adapter(
    spec: CommandSpec,
    profile: ModelProfile,
    request: dict[str, Any],
    config: ProjectConfig,
) -> tuple[dict[str, Any], dict[str, Any]]:
    command = [part.format(model=profile.model) for part in spec.command]
    environment = dict(os.environ)
    environment.update(config.environment)
    environment["ODIN_PROJECT_ROOT"] = str(config.root)
    started = time.perf_counter()
    try:
        completed = subprocess.run(
            command,
            cwd=config.root,
            env=environment,
            input=json.dumps(request),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=spec.timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise AdapterError(f"adapter '{profile.adapter}' failed to run: {exc}") from exc
    metadata = {
        "command": command,
        "exit_code": completed.returncode,
        "stderr": completed.stderr,
        "model_profile": profile.name,
        "model": profile.model,
        "parameter_billions": profile.parameter_billions,
        "duration_seconds": round(time.perf_counter() - started, 6),
    }
    if completed.returncode != 0:
        raise AdapterError(
            f"adapter '{profile.adapter}' exited {completed.returncode}: "
            f"{completed.stderr.strip()}"
        )
    try:
        response = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise AdapterError(
            f"adapter '{profile.adapter}' returned invalid JSON: {exc}"
        ) from exc
    if not isinstance(response, dict):
        raise AdapterError(f"adapter '{profile.adapter}' must return a JSON object")
    return response, metadata
