"""Runtime discovery of reachable model providers.

Odin does not bundle a provider and does not hardcode model identifiers. It
probes the local machine for endpoints and agent CLIs that are actually
reachable right now, and reports what it finds. Configuration is then written
against observed reality instead of guessed names.

Every probe is short-timeout, read-only, and standard library only, so
`odin doctor` stays fast enough for a GUI to call on demand.
"""

from __future__ import annotations

import json
import os
import shutil
import socket
import subprocess
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

from .environment import build_child_environment
from typing import Any

PROBE_TIMEOUT_SECONDS = 1.5

# One probe covers most of the ecosystem: Ollama, LM Studio, llama.cpp,
# vLLM, LiteLLM, OpenRouter, OpenAI and Azure all expose GET /v1/models.
DEFAULT_ENDPOINTS: tuple[tuple[str, str], ...] = (
    ("ollama", "http://127.0.0.1:11434/v1"),
    ("lm-studio", "http://127.0.0.1:1234/v1"),
    ("llama-cpp", "http://127.0.0.1:8080/v1"),
    ("vllm", "http://127.0.0.1:8000/v1"),
    ("litellm", "http://127.0.0.1:4000/v1"),
)

# Agent CLIs that can act as an adapter target via adapters/cli_agent.py.
KNOWN_AGENT_CLIS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("opencode", ("models",)),
    ("claude", ("--version",)),
    ("aider", ("--version",)),
    ("codex", ("--version",)),
    ("llm", ("models",)),
)

# Hosted providers are "linkable" when their credential is present. Odin stores
# the variable name, never the value.
HOSTED_KEYS: tuple[tuple[str, str, str], ...] = (
    ("openai", "OPENAI_API_KEY", "https://api.openai.com/v1"),
    ("anthropic", "ANTHROPIC_API_KEY", "https://api.anthropic.com/v1"),
    ("openrouter", "OPENROUTER_API_KEY", "https://openrouter.ai/api/v1"),
    ("groq", "GROQ_API_KEY", "https://api.groq.com/openai/v1"),
    ("together", "TOGETHER_API_KEY", "https://api.together.xyz/v1"),
    ("mistral", "MISTRAL_API_KEY", "https://api.mistral.ai/v1"),
    ("deepseek", "DEEPSEEK_API_KEY", "https://api.deepseek.com/v1"),
)


@dataclass
class DiscoveredModel:
    id: str
    provider: str
    transport: str
    base_url: str | None = None
    api_key_env: str | None = None
    parameter_billions: float | None = None
    context_tokens: int | None = None
    detail: dict[str, Any] = field(default_factory=dict)

    def as_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "provider": self.provider,
            "transport": self.transport,
            "base_url": self.base_url,
            "api_key_env": self.api_key_env,
            "parameter_billions": self.parameter_billions,
            "context_tokens": self.context_tokens,
            "detail": self.detail,
        }


@dataclass
class DiscoveredProvider:
    name: str
    transport: str
    status: str
    detail: str = ""
    base_url: str | None = None
    api_key_env: str | None = None
    command: str | None = None
    models: list[DiscoveredModel] = field(default_factory=list)

    def as_dict(self) -> dict[str, Any]:
        return {
            "name": self.name,
            "transport": self.transport,
            "status": self.status,
            "detail": self.detail,
            "base_url": self.base_url,
            "api_key_env": self.api_key_env,
            "command": self.command,
            "models": [model.as_dict() for model in self.models],
        }


def _port_open(host: str, port: int, timeout: float = 0.35) -> bool:
    """Cheap pre-check so a closed port costs milliseconds, not the full timeout."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def _http_json(url: str, token: str | None = None, timeout: float = PROBE_TIMEOUT_SECONDS) -> Any:
    request = urllib.request.Request(url, headers={"Accept": "application/json"})
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8", errors="replace"))


def parse_openai_models(payload: Any) -> list[str]:
    """Extract model ids from an OpenAI-compatible /models response."""
    if not isinstance(payload, dict):
        return []
    data = payload.get("data")
    if not isinstance(data, list):
        return []
    identifiers = []
    for entry in data:
        if isinstance(entry, dict) and isinstance(entry.get("id"), str):
            identifiers.append(entry["id"])
    return sorted(identifiers)


def parse_parameter_billions(text: str | None) -> float | None:
    """Turn Ollama's '32.8B' / '7B' parameter_size string into a number."""
    if not isinstance(text, str):
        return None
    cleaned = text.strip().upper().removesuffix("B")
    try:
        return float(cleaned)
    except ValueError:
        return None


def parse_ollama_tags(payload: Any) -> list[DiscoveredModel]:
    """Ollama's native endpoint reports parameter size and quantization."""
    if not isinstance(payload, dict):
        return []
    models = payload.get("models")
    if not isinstance(models, list):
        return []
    discovered: list[DiscoveredModel] = []
    for entry in models:
        if not isinstance(entry, dict):
            continue
        name = entry.get("name") or entry.get("model")
        if not isinstance(name, str):
            continue
        details = entry.get("details") if isinstance(entry.get("details"), dict) else {}
        discovered.append(
            DiscoveredModel(
                id=name,
                provider="ollama",
                transport="openai-compatible",
                base_url="http://127.0.0.1:11434/v1",
                parameter_billions=parse_parameter_billions(details.get("parameter_size")),
                detail={
                    "quantization": details.get("quantization_level"),
                    "family": details.get("family"),
                },
            )
        )
    return sorted(discovered, key=lambda model: model.id)


def probe_openai_endpoint(name: str, base_url: str, api_key_env: str | None = None) -> DiscoveredProvider:
    provider = DiscoveredProvider(
        name=name, transport="openai-compatible", status="unreachable",
        base_url=base_url, api_key_env=api_key_env,
    )
    host_port = base_url.split("//", 1)[-1].split("/", 1)[0]
    host, _, port_text = host_port.partition(":")
    if host in {"127.0.0.1", "localhost"} and port_text.isdigit():
        if not _port_open(host, int(port_text)):
            provider.detail = "no listener on port"
            return provider
    token = os.environ.get(api_key_env) if api_key_env else None
    try:
        payload = _http_json(f"{base_url.rstrip('/')}/models", token)
    except urllib.error.HTTPError as error:
        provider.status = "auth-required" if error.code in (401, 403) else "error"
        provider.detail = f"HTTP {error.code}"
        return provider
    except (urllib.error.URLError, OSError, json.JSONDecodeError) as error:
        provider.detail = str(error)
        return provider
    provider.status = "ready"
    provider.models = [
        DiscoveredModel(
            id=identifier, provider=name, transport="openai-compatible",
            base_url=base_url, api_key_env=api_key_env,
        )
        for identifier in parse_openai_models(payload)
    ]
    provider.detail = f"{len(provider.models)} model(s)"
    return provider


def probe_ollama_native() -> DiscoveredProvider | None:
    if not _port_open("127.0.0.1", 11434):
        return None
    provider = DiscoveredProvider(
        name="ollama", transport="openai-compatible", status="unreachable",
        base_url="http://127.0.0.1:11434/v1",
    )
    try:
        payload = _http_json("http://127.0.0.1:11434/api/tags")
    except (urllib.error.URLError, OSError, json.JSONDecodeError) as error:
        provider.detail = str(error)
        return provider
    provider.status = "ready"
    provider.models = parse_ollama_tags(payload)
    provider.detail = f"{len(provider.models)} model(s), native tags"
    return provider


def _candidate_dirs(project_root: Path | None = None) -> list[Path]:
    """Directories that commonly hold agent CLIs but are often absent from PATH.

    Desktop installers, npm global prefixes, Scoop/WinGet shims and per-user
    bin directories frequently do not export onto PATH for non-login shells, so
    a PATH-only probe reports 'not installed' for tools that are installed.
    """
    home = Path.home()
    project = Path(project_root) if project_root is not None else Path.cwd()
    candidates: list[Path] = [project / "node_modules" / ".bin"]
    tools = project / ".odin" / "tools"
    if tools.is_dir():
        candidates.append(tools)
        candidates.extend(tools.glob("*/node_modules/.bin"))
    if os.name == "nt":
        local = Path(os.environ.get("LOCALAPPDATA", home / "AppData/Local"))
        roaming = Path(os.environ.get("APPDATA", home / "AppData/Roaming"))
        candidates += [
            roaming / "npm",
            local / "Microsoft" / "WinGet" / "Links",
            home / "scoop" / "shims",
            local / "Programs",
        ]
    else:
        candidates += [
            home / ".local" / "bin",
            Path("/usr/local/bin"),
            Path("/opt/homebrew/bin"),
        ]
    return [directory for directory in candidates if directory.is_dir()]


def find_executable(
    name: str,
    extra_paths: tuple[str, ...] = (),
    project_root: Path | None = None,
) -> tuple[str, str] | None:
    """Locate an executable. Returns (path, source) or None.

    `source` is 'PATH' when the shell would find it, otherwise the directory
    that supplied it, so `doctor` can tell the user their tool exists but needs
    an absolute path in configuration.
    """
    found = shutil.which(name)
    if found:
        return found, "PATH"

    suffixes = (".cmd", ".exe", ".bat", "") if os.name == "nt" else ("",)
    search = [Path(path) for path in extra_paths] + _candidate_dirs(project_root)
    for directory in search:
        for suffix in suffixes:
            candidate = directory / f"{name}{suffix}"
            if candidate.is_file():
                return str(candidate), str(directory)
    return None


def probe_agent_cli(
    name: str,
    enumerate_args: tuple[str, ...],
    deep: bool,
    extra_paths: tuple[str, ...] = (),
    project_root: Path | None = None,
) -> DiscoveredProvider | None:
    located = find_executable(name, extra_paths, project_root)
    if located is None:
        return None
    executable, source = located
    provider = DiscoveredProvider(
        name=name, transport="cli-agent", status="ready",
        command=executable,
        detail="on PATH" if source == "PATH" else f"found in {source}, not on PATH",
    )
    if not deep:
        provider.detail += "; run with --deep to enumerate models"
        return provider
    try:
        completed = subprocess.run(
            [executable, *enumerate_args],
            env=build_child_environment(),
            capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=20,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        provider.detail = f"enumeration failed: {error}"
        return provider
    if completed.returncode != 0:
        provider.status = "auth-required"
        provider.detail = (completed.stderr or completed.stdout or "").strip()[:200]
        return provider
    identifiers = [
        line.strip() for line in completed.stdout.splitlines()
        if line.strip() and "/" in line.strip() and " " not in line.strip()
    ]
    provider.models = [
        DiscoveredModel(id=identifier, provider=name, transport="cli-agent")
        for identifier in identifiers
    ]
    provider.detail = f"{len(provider.models)} model(s)" if identifiers else "reachable"
    return provider


def discover(
    deep: bool = False,
    include_hosted: bool = True,
    extra_paths: tuple[str, ...] = (),
    project_root: Path | None = None,
) -> list[DiscoveredProvider]:
    """Probe every known transport and return what is reachable right now."""
    providers: list[DiscoveredProvider] = []

    native_ollama = probe_ollama_native()
    if native_ollama is not None:
        providers.append(native_ollama)

    for name, base_url in DEFAULT_ENDPOINTS:
        if name == "ollama" and native_ollama is not None:
            continue
        provider = probe_openai_endpoint(name, base_url)
        if provider.status != "unreachable" or provider.detail != "no listener on port":
            providers.append(provider)

    custom = os.environ.get("OPENAI_BASE_URL")
    if custom:
        providers.append(probe_openai_endpoint("custom", custom, "OPENAI_API_KEY"))

    if include_hosted:
        for name, key_env, base_url in HOSTED_KEYS:
            if os.environ.get(key_env):
                providers.append(probe_openai_endpoint(name, base_url, key_env))

    for name, enumerate_args in KNOWN_AGENT_CLIS:
        provider = probe_agent_cli(name, enumerate_args, deep, extra_paths, project_root)
        if provider is not None:
            providers.append(provider)

    return providers


def emit_config(providers: list[DiscoveredProvider], limit: int = 6) -> str:
    """Render reachable models as paste-ready odin.toml blocks."""
    lines: list[str] = []
    seen_adapters: set[str] = set()
    profiles: list[str] = []
    adapter_root = Path(__file__).resolve().parent.parent / "adapters"
    python = json.dumps(sys.executable)
    http_adapter = json.dumps(str(adapter_root / "openai_compatible.py"))
    cli_adapter = json.dumps(str(adapter_root / "cli_agent.py"))

    for provider in providers:
        if provider.status != "ready" or not provider.models:
            continue
        if provider.transport == "openai-compatible":
            adapter = f"http-{provider.name}"
            if adapter not in seen_adapters:
                seen_adapters.add(adapter)
                key_argument = (
                    f',\n  "--api-key-env", "{provider.api_key_env}"' if provider.api_key_env else ""
                )
                lines.append(
                    f"[adapters.{adapter}]\n"
                    f"command = [\n"
                    f"  {python}, {http_adapter},\n"
                    f'  "--model", "{{model}}",\n'
                    f'  "--base-url", "{provider.base_url}"{key_argument}\n'
                    f"]\n"
                    + (f'inherit_environment = ["{provider.api_key_env}"]\n' if provider.api_key_env else "")
                    + "timeout_seconds = 900\n"
                )
        else:
            adapter = f"cli-{provider.name}"
            if adapter not in seen_adapters:
                seen_adapters.add(adapter)
                lines.append(
                    f"[adapters.{adapter}]\n"
                    f"command = [\n"
                    f"  {python}, {cli_adapter},\n"
                    f'  "--model", "{{model}}",\n'
                    f'  "--", {json.dumps(provider.command or provider.name)}, "run", '
                    f'"--model", "{{model}}"\n'
                    f"]\ntimeout_seconds = 1200\n"
                )
        for model in provider.models[:limit]:
            profile = model.id.replace("/", "-").replace(":", "-").replace(".", "-")
            size = (
                f"parameter_billions = {model.parameter_billions}\n"
                if model.parameter_billions is not None else ""
            )
            profiles.append(
                f"[models.{profile}]\n"
                f'adapter = "{adapter}"\n'
                f'model = "{model.id}"\n'
                f"{size}"
                f'tags = ["{provider.name}", "discovered"]\n'
            )

    if not lines and not profiles:
        return "# No reachable providers were discovered.\n"
    return "\n".join(lines + profiles)
