from __future__ import annotations

from pathlib import Path
from typing import Any

from .contracts import validate
from .errors import WorkflowError
from .io import read_json


PACKAGE_ROOT = Path(__file__).resolve().parent


def _load(kind: str, identifier: str, contract: str, enforce_identifier: bool = True) -> dict[str, Any]:
    path = PACKAGE_ROOT / kind / f"{identifier}.json"
    try:
        value = read_json(path)
    except ValueError as exc:
        raise WorkflowError(str(exc)) from exc
    validate(value, contract, str(path))
    if enforce_identifier and value.get("id") != identifier:
        raise WorkflowError(f"{path}: id '{value.get('id')}' does not match filename '{identifier}'")
    return value


def load_agent(identifier: str) -> dict[str, Any]:
    return _load("agents", identifier, "agent")


def load_skill(identifier: str) -> dict[str, Any]:
    return _load("skills", identifier, "skill")


def load_workflow(identifier: str) -> dict[str, Any]:
    return _load("workflows", identifier, "workflow")


def load_template(identifier: str) -> dict[str, Any]:
    return _load("templates", identifier, "task", enforce_identifier=False)
