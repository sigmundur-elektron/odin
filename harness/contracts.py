from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .errors import ContractError

try:
    from jsonschema import Draft202012Validator
except ModuleNotFoundError:  # pragma: no cover - exercised by deployment, not tests
    Draft202012Validator = None


PACKAGE_ROOT = Path(__file__).resolve().parent
SCHEMA_ROOT = PACKAGE_ROOT / "schemas"


def load_schema(name: str) -> dict[str, Any]:
    path = SCHEMA_ROOT / f"{name}.schema.json"
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ContractError(f"unknown contract: {name}") from exc


def validate(value: Any, contract: str, where: str = "value") -> None:
    if Draft202012Validator is None:
        raise ContractError(
            "jsonschema is required; install with: "
            "python -m pip install -r scripts/requirements.txt"
        )
    validator = Draft202012Validator(load_schema(contract))
    errors = sorted(validator.iter_errors(value), key=lambda error: list(error.absolute_path))
    if not errors:
        return
    details = []
    for error in errors:
        location = "/".join(str(part) for part in error.absolute_path) or "<root>"
        details.append(f"{location}: {error.message}")
    raise ContractError(f"{where} violates {contract}: " + "; ".join(details))
