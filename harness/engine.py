from __future__ import annotations

import datetime as dt
import os
import subprocess
import uuid
from pathlib import Path
from typing import Any

from .adapters import run_command_adapter
from .config import CommandSpec, ProjectConfig
from .contracts import validate
from .definitions import load_agent, load_skill, load_workflow
from .errors import AdapterError, WorkflowError
from .io import read_json, write_json_atomic


TERMINAL = {"complete", "blocked", "failed"}


def _utc_now() -> str:
    return dt.datetime.now(dt.UTC).isoformat(timespec="seconds")


def _expand(value: str, context: dict[str, Any]) -> str:
    task = context.get("task", {})
    return value.format(
        task_file=context.get("task_file", ""),
        run_dir=context.get("run_dir", ""),
        task_id=task.get("id", ""),
        kind=task.get("kind", ""),
    )


def _run_gate(name: str, spec: CommandSpec, config: ProjectConfig, context: dict[str, Any]) -> dict[str, Any]:
    command = [_expand(part, context) for part in spec.command]
    environment = dict(os.environ)
    environment.update(config.environment)
    try:
        completed = subprocess.run(
            command,
            cwd=config.root,
            env=environment,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=spec.timeout_seconds,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {
            "status": "failed",
            "summary": f"gate '{name}' could not run: {exc}",
            "command": command,
            "exit_code": None,
            "output": "",
        }
    return {
        "status": "passed" if completed.returncode == 0 else "failed",
        "summary": f"gate '{name}' exited {completed.returncode}",
        "command": command,
        "exit_code": completed.returncode,
        "output": completed.stdout,
    }


class WorkflowEngine:
    def __init__(self, config: ProjectConfig) -> None:
        self.config = config

    def create_run(self, task: dict[str, Any], task_file: Path) -> Path:
        validate(task, "task", str(task_file))
        workflow = load_workflow(task["kind"])
        run_id = f"{dt.datetime.now().strftime('%Y%m%d-%H%M%S')}-{task['id']}-{uuid.uuid4().hex[:6]}"
        run_dir = self.config.state_dir / run_id
        context = {
            "run_id": run_id,
            "run_dir": str(run_dir.relative_to(self.config.root)),
            "task_file": str(task_file.resolve()),
            "task": task,
            "artifacts": {},
            "history": [],
        }
        state = {
            "schema_version": 1,
            "run_id": run_id,
            "workflow": workflow["id"],
            "status": "running",
            "current_stage": workflow["start"],
            "transitions": 0,
            "stage_attempts": {},
            "created_at": _utc_now(),
            "updated_at": _utc_now(),
        }
        write_json_atomic(run_dir / "task.json", task)
        write_json_atomic(run_dir / "context.json", context)
        write_json_atomic(run_dir / "state.json", state)
        return run_dir

    def run(self, run_dir: Path, model_override: str | None = None) -> dict[str, Any]:
        state = read_json(run_dir / "state.json")
        context = read_json(run_dir / "context.json")
        workflow = load_workflow(state["workflow"])
        stages = {stage["id"]: stage for stage in workflow["stages"]}

        while state["status"] not in TERMINAL:
            if state["transitions"] >= self.config.max_total_transitions:
                state["status"] = "blocked"
                state["reason"] = "maximum total transitions reached"
                break
            stage_id = state["current_stage"]
            try:
                stage = stages[stage_id]
            except KeyError as exc:
                raise WorkflowError(f"workflow references unknown stage '{stage_id}'") from exc
            attempts = state["stage_attempts"].get(stage_id, 0) + 1
            state["stage_attempts"][stage_id] = attempts
            if attempts > stage.get("max_attempts", 3):
                state["status"] = "blocked"
                state["reason"] = f"stage '{stage_id}' exceeded its attempt limit"
                break

            result, metadata = self._execute_stage(stage, context, model_override)
            validate(result, "handoff", f"stage '{stage_id}' output")
            record = {
                "sequence": len(context["history"]) + 1,
                "stage": stage_id,
                "kind": stage["kind"],
                "attempt": attempts,
                "at": _utc_now(),
                "result": result,
                "metadata": metadata,
            }
            context["history"].append(record)
            artifact_name = stage.get("output", stage_id)
            context["artifacts"][artifact_name] = result
            write_json_atomic(run_dir / "events" / f"{record['sequence']:03d}-{stage_id}.json", record)

            outcome = result["status"]
            transitions = stage.get("on", {})
            next_stage = transitions.get(outcome)
            if not next_stage:
                state["status"] = "blocked"
                state["reason"] = f"stage '{stage_id}' has no transition for '{outcome}'"
                break
            state["transitions"] += 1
            if next_stage in TERMINAL:
                state["status"] = next_stage
                state["current_stage"] = stage_id
            else:
                state["current_stage"] = next_stage
            state["updated_at"] = _utc_now()
            write_json_atomic(run_dir / "context.json", context)
            write_json_atomic(run_dir / "state.json", state)

        state["updated_at"] = _utc_now()
        write_json_atomic(run_dir / "context.json", context)
        write_json_atomic(run_dir / "state.json", state)
        return state

    def _execute_stage(
        self,
        stage: dict[str, Any],
        context: dict[str, Any],
        model_override: str | None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        kind = stage["kind"]
        if kind == "agent":
            return self._run_agent(stage, context, model_override)
        if kind == "gate":
            gate_name = stage["gate"]
            try:
                gate = self.config.gates[gate_name]
            except KeyError as exc:
                raise WorkflowError(f"gate '{gate_name}' is not configured") from exc
            gate_result = _run_gate(gate_name, gate, self.config, context)
            status = "approved" if gate_result["status"] == "passed" else "revision"
            return {
                "status": status,
                "summary": gate_result["summary"],
                "artifacts": {"gate": gate_result},
                "findings": [] if status == "approved" else [gate_result["summary"]],
            }, {"gate": gate_name}
        if kind == "checkpoint":
            required = stage.get("requires", [])
            missing = [name for name in required if name not in context["artifacts"]]
            if missing:
                return {
                    "status": "blocked",
                    "summary": "checkpoint is missing required artifacts",
                    "artifacts": {"missing": missing},
                    "findings": [f"missing artifact: {name}" for name in missing],
                }, {"checkpoint": stage["id"]}
            return {
                "status": "approved",
                "summary": f"checkpoint '{stage['id']}' satisfied",
                "artifacts": {"required": required},
                "findings": [],
            }, {"checkpoint": stage["id"]}
        if kind == "stage":
            paths = context.get("artifacts", {}).get(stage.get("paths_from", "implementation"), {})
            candidates = paths.get("artifacts", {}).get("changed_files", [])
            if not isinstance(candidates, list) or not candidates:
                return {
                    "status": "blocked",
                    "summary": "no explicit changed_files artifact was supplied for staging",
                    "artifacts": {},
                    "findings": ["explicit staging requires a non-empty changed_files array"],
                }, {"operation": "git-stage"}
            if not self.config.stage_on_success:
                return {
                    "status": "approved",
                    "summary": "explicit staging manifest prepared; automatic staging is disabled",
                    "artifacts": {"staged_files": [], "staging_manifest": candidates},
                    "findings": [],
                }, {"operation": "git-stage", "executed": False}
            completed = subprocess.run(
                ["git", "add", "--", *candidates],
                cwd=self.config.root,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            return {
                "status": "approved" if completed.returncode == 0 else "blocked",
                "summary": f"explicit git staging exited {completed.returncode}",
                "artifacts": {"staged_files": candidates, "output": completed.stdout},
                "findings": [] if completed.returncode == 0 else [completed.stdout.strip()],
            }, {"operation": "git-stage", "exit_code": completed.returncode}
        raise WorkflowError(f"unsupported stage kind: {kind}")

    def _run_agent(
        self,
        stage: dict[str, Any],
        context: dict[str, Any],
        model_override: str | None,
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        agent = load_agent(stage["agent"])
        skills = [load_skill(identifier) for identifier in agent.get("skills", [])]
        profile = self.config.model_for(agent["id"], model_override)
        adapter = self.config.adapter_for(profile)
        request = {
            "contract": "handoff/v1",
            "agent": agent,
            "skills": skills,
            "stage": {key: value for key, value in stage.items() if key != "on"},
            "task": context["task"],
            "artifacts": context["artifacts"],
            "required_output": {
                "status": "approved | revision | blocked",
                "summary": "string",
                "artifacts": "object",
                "findings": "string[]",
            },
        }
        try:
            return run_command_adapter(adapter, profile, request, self.config)
        except AdapterError as exc:
            return {
                "status": "blocked",
                "summary": str(exc),
                "artifacts": {},
                "findings": [str(exc)],
            }, {"adapter": profile.adapter, "model_profile": profile.name}
