"""Optional agent-CLI installation, kept out of the default path.

Odin never installs a vendor CLI on its own. When a user explicitly asks, tools
are installed under `.odin/tools/<name>/`, which is gitignored and already
searched by discovery, so nothing lands in the repository root and nothing
appears in `git status`.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

# Only tools with a known, non-interactive install command are offered.
KNOWN_TOOLS: dict[str, dict[str, str]] = {
    "opencode": {
        "package": "opencode-ai",
        "manager": "npm",
        "binary": "opencode",
        "description": "OpenCode CLI; reuses an existing OpenCode login if present",
    },
    "aider": {
        "package": "aider-chat",
        "manager": "pip",
        "binary": "aider",
        "description": "Aider coding agent",
    },
    "llm": {
        "package": "llm",
        "manager": "pip",
        "binary": "llm",
        "description": "Simon Willison's llm CLI, many provider plugins",
    },
}


class ToolError(Exception):
    """A tool could not be installed."""


def tools_root(root: Path) -> Path:
    return Path(root) / ".odin" / "tools"


def installed(root: Path) -> dict[str, str]:
    """Map tool name to the resolved binary path, for tools installed here."""
    found: dict[str, str] = {}
    base = tools_root(root)
    if not base.is_dir():
        return found
    for name, spec in KNOWN_TOOLS.items():
        target = base / name
        for candidate in (
            target / "node_modules" / ".bin" / f"{spec['binary']}.cmd",
            target / "node_modules" / ".bin" / spec["binary"],
            target / "Scripts" / f"{spec['binary']}.exe",
            target / "bin" / spec["binary"],
        ):
            if candidate.is_file():
                found[name] = str(candidate)
                break
    return found


def install(root: Path, name: str, timeout: int = 900) -> str:
    spec = KNOWN_TOOLS.get(name)
    if spec is None:
        raise ToolError(
            f"unknown tool '{name}'. Known: {', '.join(sorted(KNOWN_TOOLS))}"
        )
    target = tools_root(root) / name
    target.mkdir(parents=True, exist_ok=True)

    if spec["manager"] == "npm":
        if shutil.which("npm") is None:
            raise ToolError("npm is required to install this tool but was not found on PATH")
        command = ["npm", "install", "--prefix", str(target), spec["package"]]
    else:
        command = ["python", "-m", "pip", "install", "--target", str(target), spec["package"]]

    try:
        completed = subprocess.run(
            command, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=timeout,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ToolError(f"install failed: {error}") from error
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()[:400]
        raise ToolError(f"{' '.join(command)} exited {completed.returncode}: {detail}")

    resolved = installed(root).get(name)
    if resolved is None:
        raise ToolError(
            f"'{name}' installed but its binary was not found under {target}"
        )
    return resolved
