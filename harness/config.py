from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .errors import WorkflowError


@dataclass(frozen=True)
class CommandSpec:
    command: list[str]
    timeout_seconds: int = 300


@dataclass(frozen=True)
class ModelProfile:
    name: str
    adapter: str
    model: str
    parameter_billions: float | None = None
    context_tokens: int | None = None
    tags: tuple[str, ...] = ()


@dataclass(frozen=True)
class ProjectConfig:
    root: Path
    state_dir: Path
    adapters: dict[str, CommandSpec]
    gates: dict[str, CommandSpec]
    models: dict[str, ModelProfile]
    routing: dict[str, str]
    stage_on_success: bool = False
    max_total_transitions: int = 40
    environment: dict[str, str] = field(default_factory=dict)

    def model_for(self, agent_id: str, override: str | None = None) -> ModelProfile:
        name = override or self.routing.get(agent_id) or self.routing.get("default")
        if not name:
            raise WorkflowError(f"no model route configured for agent '{agent_id}'")
        try:
            return self.models[name]
        except KeyError as exc:
            raise WorkflowError(f"model profile '{name}' does not exist") from exc

    def adapter_for(self, profile: ModelProfile) -> CommandSpec:
        try:
            return self.adapters[profile.adapter]
        except KeyError as exc:
            raise WorkflowError(
                f"adapter '{profile.adapter}' for model profile '{profile.name}' does not exist"
            ) from exc


def _command_spec(name: str, raw: Any) -> CommandSpec:
    if not isinstance(raw, dict):
        raise WorkflowError(f"command '{name}' must be a table")
    command = raw.get("command")
    if not isinstance(command, list) or not command or not all(isinstance(x, str) for x in command):
        raise WorkflowError(f"command '{name}' must contain a non-empty string array 'command'")
    timeout = raw.get("timeout_seconds", 300)
    if not isinstance(timeout, int) or timeout < 1:
        raise WorkflowError(f"command '{name}'.timeout_seconds must be a positive integer")
    return CommandSpec(command, timeout)


def load_config(path: Path) -> ProjectConfig:
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise WorkflowError(f"configuration not found: {path}") from exc
    except tomllib.TOMLDecodeError as exc:
        raise WorkflowError(f"invalid TOML in {path}: {exc}") from exc

    root = path.resolve().parent
    harness = raw.get("harness", {})
    state_dir = root / harness.get("state_dir", ".odin/runs")
    maximum = harness.get("max_total_transitions", 40)
    if not isinstance(maximum, int) or maximum < 1:
        raise WorkflowError("harness.max_total_transitions must be a positive integer")

    adapters = {
        name: _command_spec(f"adapters.{name}", value)
        for name, value in raw.get("adapters", {}).items()
    }
    gates = {
        name: _command_spec(f"gates.{name}", value)
        for name, value in raw.get("gates", {}).items()
    }

    models: dict[str, ModelProfile] = {}
    for name, value in raw.get("models", {}).items():
        if not isinstance(value, dict):
            raise WorkflowError(f"models.{name} must be a table")
        adapter = value.get("adapter")
        model = value.get("model")
        if not isinstance(adapter, str) or not isinstance(model, str):
            raise WorkflowError(f"models.{name} requires string adapter and model values")
        size = value.get("parameter_billions")
        context = value.get("context_tokens")
        tags = value.get("tags", [])
        if size is not None and not isinstance(size, (int, float)):
            raise WorkflowError(f"models.{name}.parameter_billions must be numeric")
        if context is not None and not isinstance(context, int):
            raise WorkflowError(f"models.{name}.context_tokens must be an integer")
        if not isinstance(tags, list) or not all(isinstance(tag, str) for tag in tags):
            raise WorkflowError(f"models.{name}.tags must be a string array")
        models[name] = ModelProfile(name, adapter, model, size, context, tuple(tags))

    routing = raw.get("routing", {})
    if not isinstance(routing, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in routing.items()
    ):
        raise WorkflowError("routing must be a string-to-string table")

    git = raw.get("git", {})
    environment = raw.get("environment", {})
    if not isinstance(environment, dict) or not all(
        isinstance(key, str) and isinstance(value, str) for key, value in environment.items()
    ):
        raise WorkflowError("environment must be a string-to-string table")

    return ProjectConfig(
        root=root,
        state_dir=state_dir,
        adapters=adapters,
        gates=gates,
        models=models,
        routing=dict(routing),
        stage_on_success=bool(git.get("stage_on_success", False)),
        max_total_transitions=maximum,
        environment=dict(environment),
    )
