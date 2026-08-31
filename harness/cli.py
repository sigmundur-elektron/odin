from __future__ import annotations

import argparse
import datetime as dt
import getpass
import json
import sys
import time
from pathlib import Path

from .config import load_config
from .contracts import validate
from .credentials import CredentialStore
from .definitions import load_agent, load_skill, load_template, load_workflow
from .discovery import discover, emit_config
from .engine import WorkflowEngine
from .errors import OdinError
from .io import read_json, write_json_atomic
from .tools import KNOWN_TOOLS, ToolError, install, installed


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="odin", description="Contract-driven development workflow harness")
    parser.add_argument("--config", default="odin.toml", help="project configuration (default: odin.toml)")
    commands = parser.add_subparsers(dest="command", required=True)

    start = commands.add_parser("start", help="run a feature or bug-fix template")
    start.add_argument("template", help="template JSON path, or built-in name: feature / bugfix")
    start.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="override a top-level template value")
    start.add_argument("--model", help="force one configured model profile for all agent stages")

    resume = commands.add_parser("resume", help="continue an existing run")
    resume.add_argument("run", help="run id or run directory")
    resume.add_argument("--model", help="force one configured model profile for all agent stages")

    status = commands.add_parser("status", help="show one run's durable state")
    status.add_argument("run", help="run id or run directory")

    benchmark = commands.add_parser("benchmark", help="compare configured model profiles on one task")
    benchmark.add_argument("template", help="template JSON path, or built-in name")
    benchmark.add_argument("--models", nargs="+", required=True, help="configured model profile names")
    benchmark.add_argument("--set", action="append", default=[], metavar="KEY=VALUE")

    validate_command = commands.add_parser("validate", help="validate config, workflows, agents, skills, and templates")
    validate_command.add_argument("--self-only", action="store_true", help="validate bundled definitions without loading project configuration")

    doctor = commands.add_parser("doctor", help="probe the machine for reachable model providers")
    doctor.add_argument("--deep", action="store_true", help="also enumerate models from agent CLIs (slower)")
    doctor.add_argument("--no-hosted", action="store_true", help="skip hosted providers even when their key is set")
    doctor.add_argument("--json", action="store_true", help="machine-readable output for tooling and GUIs")
    doctor.add_argument("--emit-config", action="store_true", help="print paste-ready odin.toml blocks for what was found")
    doctor.add_argument("--path", action="append", default=[], metavar="DIR", help="extra directory to search for agent CLIs")

    models = commands.add_parser("models", help="list configured model profiles")
    models.add_argument("--json", action="store_true")

    auth = commands.add_parser("auth", help="store provider credentials for agents and subagents")
    auth_commands = auth.add_subparsers(dest="auth_command", required=True)

    auth_set = auth_commands.add_parser("set", help="store an API key (prompts without echo)")
    auth_set.add_argument("name", help="credential name, referenced by --credential in adapters")
    auth_set.add_argument("--value", help="the secret; omit to be prompted without echo")
    auth_set.add_argument("--stdin", action="store_true", help="read the secret from stdin")
    auth_set.add_argument("--note", help="optional reminder of what this credential is for")

    auth_list = auth_commands.add_parser("list", help="list stored credentials, values masked")
    auth_list.add_argument("--json", action="store_true")

    auth_remove = auth_commands.add_parser("remove", help="delete a stored credential")
    auth_remove.add_argument("name")

    auth_import = auth_commands.add_parser(
        "import", help="import an existing OAuth token from another tool's credential file"
    )
    auth_import.add_argument("--from-file", required=True, help="path to the source credentials JSON")
    auth_import.add_argument("--provider", required=True, help="provider key inside that file")
    auth_import.add_argument("--name", help="name to store it under (defaults to --provider)")

    tools = commands.add_parser("tools", help="optionally install an agent CLI into .odin/tools")
    tools_commands = tools.add_subparsers(dest="tools_command", required=True)
    tools_commands.add_parser("list", help="show known and installed tools")
    tools_install = tools_commands.add_parser("install", help="install a known agent CLI")
    tools_install.add_argument("name")
    return parser


def _overrides(items: list[str]) -> dict[str, str]:
    result = {}
    for item in items:
        key, separator, value = item.partition("=")
        if not separator or not key:
            raise OdinError(f"invalid --set value '{item}', expected KEY=VALUE")
        result[key] = value
    return result


def _task(template: str, changes: dict[str, str]) -> tuple[dict, Path]:
    source = Path(template)
    if source.exists():
        value = read_json(source)
        path = source.resolve()
    else:
        value = load_template(template)
        path = Path(f"built-in:{template}")
    value.update(changes)
    validate(value, "task", str(path))
    return value, path


def _run_dir(config, value: str) -> Path:
    candidate = Path(value)
    if candidate.is_dir():
        return candidate.resolve()
    return config.state_dir / value


def _validate_all() -> int:
    root = Path(__file__).resolve().parent
    counts = {}
    loaded = {}
    for kind, loader in (
        ("agents", load_agent),
        ("skills", load_skill),
        ("workflows", load_workflow),
        ("templates", load_template),
    ):
        identifiers = [path.stem for path in (root / kind).glob("*.json")]
        loaded[kind] = {identifier: loader(identifier) for identifier in identifiers}
        counts[kind] = len(identifiers)
    for identifier, agent in loaded["agents"].items():
        missing = sorted(set(agent["skills"]) - set(loaded["skills"]))
        if missing:
            raise OdinError(f"agent '{identifier}' references missing skills: {', '.join(missing)}")
    for identifier, workflow in loaded["workflows"].items():
        stage_ids = {stage["id"] for stage in workflow["stages"]}
        if workflow["start"] not in stage_ids:
            raise OdinError(f"workflow '{identifier}' starts at missing stage '{workflow['start']}'")
        for stage in workflow["stages"]:
            if stage.get("agent") and stage["agent"] not in loaded["agents"]:
                raise OdinError(f"workflow '{identifier}' references missing agent '{stage['agent']}'")
            invalid = sorted(
                target for target in stage["on"].values()
                if target not in stage_ids and target not in {"complete", "blocked", "failed"}
            )
            if invalid:
                raise OdinError(
                    f"workflow '{identifier}' stage '{stage['id']}' has invalid transitions: "
                    + ", ".join(invalid)
                )
    print(json.dumps({"status": "valid", "counts": counts}, indent=2))
    return 0


def _doctor(args, root: Path) -> int:
    providers = discover(
        deep=args.deep,
        include_hosted=not args.no_hosted,
        extra_paths=tuple(args.path),
        project_root=root,
    )
    stored = CredentialStore(root).describe_all()
    if args.emit_config:
        print(emit_config(providers))
        return 0
    if args.json:
        payload = {
            "providers": [provider.as_dict() for provider in providers],
            "credentials": stored,
            "ready": sum(1 for provider in providers if provider.status == "ready"),
        }
        print(json.dumps(payload, indent=2))
        return 0 if payload["ready"] else 1

    if not providers:
        print("No model providers were detected.")
        print()
        print("Odin bundles no provider and installs nothing on its own. Link one by:")
        print("  1. storing a key      odin auth set openrouter")
        print("  2. starting a server  Ollama, LM Studio, llama.cpp, vLLM, LiteLLM")
        print("  3. installing a CLI   odin tools install opencode")
        print()
        if stored:
            print(f"{len(stored)} credential(s) already stored:")
            for entry in stored:
                print(f"  - {entry['name']} ({entry['type']}, {entry['value']})")
            print("Reference one from an adapter with --credential <name>.")
            print()
        print("Then re-run: odin doctor --emit-config")
        print()
        print("Already have an agent CLI installed outside PATH? Point at it:")
        print("  odin doctor --deep --path <directory-containing-the-binary>")
        return 1

    ready = 0
    for provider in providers:
        marker = {"ready": "ok", "auth-required": "auth", "error": "err"}.get(provider.status, "--")
        target = provider.base_url or provider.command or ""
        print(f"[{marker:>4}] {provider.name:<12} {provider.transport:<19} {target}")
        if provider.detail:
            print(f"         {provider.detail}")
        for model in provider.models[:8]:
            size = f"  ~{model.parameter_billions}B" if model.parameter_billions else ""
            print(f"         - {model.id}{size}")
        if len(provider.models) > 8:
            print(f"         ... {len(provider.models) - 8} more")
        if provider.status == "ready":
            ready += 1
    print()
    print(f"{ready} provider(s) ready. Emit config with: odin doctor --emit-config")
    if stored:
        print()
        print("Stored credentials (values masked):")
        for entry in stored:
            state = "  [EXPIRED]" if entry.get("expired") is True else ""
            print(f"  - {entry['name']:<20} {entry['type']:<8} {entry['value']}{state}")
    return 0 if ready else 1


def _models(config, as_json: bool) -> int:
    profiles = [
        {
            "profile": name,
            "adapter": profile.adapter,
            "model": profile.model,
            "parameter_billions": profile.parameter_billions,
            "tags": list(profile.tags),
            "adapter_configured": profile.adapter in config.adapters,
        }
        for name, profile in sorted(config.models.items())
    ]
    routes = dict(sorted(config.routing.items()))
    if as_json:
        print(json.dumps({"models": profiles, "routing": routes}, indent=2))
        return 0
    if not profiles:
        print("No model profiles configured. Run: odin doctor --emit-config")
        return 1
    for item in profiles:
        flag = "" if item["adapter_configured"] else "  [adapter missing]"
        size = f"  ~{item['parameter_billions']}B" if item["parameter_billions"] else ""
        print(f"{item['profile']:<28} {item['adapter']:<16} {item['model']}{size}{flag}")
    print()
    for role, profile in routes.items():
        print(f"routing.{role:<20} -> {profile}")
    return 0


def _auth(args, root: Path) -> int:
    store = CredentialStore(root)

    if args.auth_command == "set":
        if args.stdin:
            secret = sys.stdin.read().strip()
        elif args.value is not None:
            secret = args.value
        else:
            secret = getpass.getpass(f"Secret for '{args.name}' (not echoed): ")
        store.set_api_key(args.name, secret, args.note)
        described = store.describe(args.name)
        print(f"stored '{args.name}' ({described['value']}) in {store.path}")
        print("Reference it from an adapter with: --credential " + args.name)
        return 0

    if args.auth_command == "list":
        entries = store.describe_all()
        if args.json:
            print(json.dumps({"credentials": entries, "store": str(store.path)}, indent=2))
            return 0
        if not entries:
            print("No credentials stored.")
            print("Add one with: odin auth set <name>")
            return 1
        for entry in entries:
            flags = ""
            if entry.get("expired") is True:
                flags = "  [EXPIRED]"
            elif entry.get("expired") is False:
                flags = "  [valid]"
            note = f"  {entry['note']}" if entry.get("note") else ""
            print(f"{entry['name']:<24} {entry['type']:<8} {entry['value']:<16}{flags}{note}")
        print()
        print(f"store: {store.path}")
        return 0

    if args.auth_command == "remove":
        if store.remove(args.name):
            print(f"removed '{args.name}'")
            return 0
        print(f"no credential named '{args.name}'", file=sys.stderr)
        return 1

    source = Path(args.from_file).expanduser()
    try:
        payload = read_json(source)
    except ValueError as error:
        raise OdinError(str(error)) from error
    entry = payload.get(args.provider)
    if not isinstance(entry, dict):
        available = ", ".join(sorted(k for k, v in payload.items() if isinstance(v, dict)))
        raise OdinError(f"provider '{args.provider}' not found in {source}. Available: {available}")
    name = args.name or args.provider
    access = entry.get("access") or entry.get("value") or entry.get("token")
    if not access:
        raise OdinError(f"'{args.provider}' in {source} has no access token field")
    if entry.get("refresh") or entry.get("expires"):
        store.set_oauth(
            name, str(access), entry.get("refresh"), entry.get("expires"),
            note=f"imported from {source.name}",
        )
    else:
        store.set_api_key(name, str(access), note=f"imported from {source.name}")
    described = store.describe(name)
    print(f"imported '{args.provider}' as '{name}' ({described['value']})")
    if described.get("expired") is True:
        print("warning: the imported token is already expired", file=sys.stderr)
    return 0


def _tools(args, root: Path) -> int:
    present = installed(root)
    if args.tools_command == "list":
        for name, spec in sorted(KNOWN_TOOLS.items()):
            location = present.get(name)
            state = location if location else "not installed"
            print(f"{name:<12} {spec['manager']:<5} {state}")
            print(f"             {spec['description']}")
        print()
        print("Odin installs nothing automatically. Install explicitly with:")
        print("  odin tools install <name>")
        return 0
    try:
        path = install(root, args.name)
    except ToolError as error:
        raise OdinError(str(error)) from error
    print(f"installed '{args.name}' at {path}")
    print("It is inside .odin/tools, which is gitignored and searched by `doctor`.")
    print("Confirm with: odin doctor --deep")
    return 0


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    root = Path(args.config).resolve().parent
    try:
        if args.command == "auth":
            return _auth(args, root)
        if args.command == "tools":
            return _tools(args, root)
        if args.command == "doctor":
            return _doctor(args, root)
        if args.command == "validate":
            if not args.self_only:
                load_config(Path(args.config))
            return _validate_all()
        config = load_config(Path(args.config))
        if args.command == "models":
            return _models(config, args.json)
        engine = WorkflowEngine(config)
        if args.command == "benchmark":
            task, task_file = _task(args.template, _overrides(args.set))
            results = []
            for model_name in args.models:
                profile = config.model_for("benchmark", model_name)
                started = time.perf_counter()
                run_dir = engine.create_run(task, task_file)
                state = engine.run(run_dir, model_name)
                results.append(
                    {
                        "profile": model_name,
                        "model": profile.model,
                        "parameter_billions": profile.parameter_billions,
                        "tags": list(profile.tags),
                        "status": state["status"],
                        "transitions": state["transitions"],
                        "duration_seconds": round(time.perf_counter() - started, 6),
                        "run_dir": str(run_dir),
                    }
                )
            stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
            report_path = config.root / ".odin" / "benchmarks" / f"{stamp}-{task['id']}.json"
            report = {"task": task, "results": results}
            write_json_atomic(report_path, report)
            print(json.dumps({"report": str(report_path), **report}, indent=2))
            return 0 if all(item["status"] == "complete" for item in results) else 2
        if args.command == "start":
            task, task_file = _task(args.template, _overrides(args.set))
            run_dir = engine.create_run(task, task_file)
            state = engine.run(run_dir, args.model)
        elif args.command == "resume":
            run_dir = _run_dir(config, args.run)
            state = engine.run(run_dir, args.model)
        else:
            run_dir = _run_dir(config, args.run)
            state = read_json(run_dir / "state.json")
        print(json.dumps({"run_dir": str(run_dir), "state": state}, indent=2))
        return 0 if state["status"] == "complete" or args.command == "status" else 2
    except (OdinError, ValueError) as exc:
        print(f"odin: {exc}", file=sys.stderr)
        return 2
