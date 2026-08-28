from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import time
from pathlib import Path

from .config import load_config
from .contracts import validate
from .definitions import load_agent, load_skill, load_template, load_workflow
from .engine import WorkflowEngine
from .errors import OdinError
from .io import read_json, write_json_atomic


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


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "validate":
            if not args.self_only:
                load_config(Path(args.config))
            return _validate_all()
        config = load_config(Path(args.config))
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
