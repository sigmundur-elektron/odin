# Odin

Odin is a framework-neutral workflow harness for C++ projects. It does not
depend on opencode, GitHub Copilot, a particular model provider, CMake, or a
specific test framework. It coordinates work through JSON contracts and runs
the commands supplied by the consuming project.

## Start one complete process

Feature:

```powershell
python odin.py start examples/feature.json
```

Bug fix:

```powershell
python odin.py start examples/bugfix.json
```

The same command creates a durable run, executes its state machine, follows
revision transitions when a checkpoint fails, and stops at `complete` or
`blocked`. Resume an interrupted run with:

```powershell
python odin.py resume <run-id>
```

Inspect a run without changing it:

```powershell
python odin.py status <run-id>
```

## Input templates

The input is ordinary JSON. The two built-in shapes are in
`harness/templates/feature.json` and `harness/templates/bugfix.json`.

```json
{
  "id": "add-export-command",
  "kind": "feature",
  "title": "Add an export command",
  "request": "Export the active document as JSON.",
  "context": {},
  "constraints": []
}
```

A bug fix additionally requires exact reproduction steps plus expected and
actual behavior. Inputs are validated by `harness/schemas/task.schema.json`
before a run starts.

## Workflow

Feature:

```text
specify -> review specification -> checkpoint -> implement -> project gate
       ^             |                            ^             |
       +-- revision -+                            +-- revision -+

project gate -> independent verification -> finalization -> staging manifest
                       |                                  -> complete
                       +-- revision -> implementation
```

Bug fixes start with reproduction and add a regression checkpoint after
verification. Every stage has an attempt limit. A failed gate goes back to
implementation; repeated failure ends in `blocked` rather than an infinite
loop.

Run state is written under `.odin/runs/<run-id>/`:

```text
task.json       immutable validated request
state.json      current stage, status, attempts, transition count
context.json    artifacts and complete event history
events/*.json   append-only stage handoffs
```

State files use atomic replacement, so an interrupted process can resume from
the last completed stage.

## Framework and model adapters

An agent adapter is any executable that:

1. reads one JSON request from standard input;
2. writes one `handoff/v1` JSON object to standard output;
3. exits nonzero when transport or provider execution fails.

The handoff contract is intentionally small:

```json
{
  "status": "approved | revision | blocked",
  "summary": "short result",
  "artifacts": {},
  "findings": []
}
```

Configure adapters and model profiles in `odin.toml`. The `{model}` token is
expanded in adapter commands:

```toml
[adapters.local]
command = ["python", "adapters/openai_compatible.py", "--model", "{model}"]

[models.coder-32b]
adapter = "local"
model = "qwen-coder-32b"
parameter_billions = 32
tags = ["local", "candidate"]

[models.frontier]
adapter = "cloud"
model = "provider/state-of-the-art-model"
tags = ["reference"]

[routing]
default = "coder-32b"
reviewer = "frontier"
```

Model size is descriptive metadata, not a fixed threshold. Use `--model` to run
the same workflow with a different configured profile. This permits controlled
comparisons between 30B, 80B, larger local models, and state-of-the-art hosted
models without changing agents, skills, or workflow definitions.

Run an apples-to-apples comparison with:

```powershell
python odin.py benchmark examples/feature.json --models coder-32b coder-80b frontier
```

Each profile gets a fresh run. Odin records completion status, transitions,
wall-clock duration, model metadata, and run artifacts under
`.odin/benchmarks/`. Adapter-specific token and cost metrics can be added to the
adapter metadata without changing workflow definitions. Odin deliberately does
not impose a minimum parameter count; benchmark evidence determines suitability.

The checked-in `mock` adapter is deterministic test infrastructure. It proves
workflow mechanics and contracts; it is not an implementation model.

## Project gates

Odin prescribes no build system. The consuming repository supplies the quality
command in `odin.toml`:

```toml
[gates.quality]
command = ["python", "scripts/gate.py"]
timeout_seconds = 900
```

It can instead be CMake/CTest, Meson, Bazel, a shell script, or a container
entrypoint. Odin records the exact command, exit code, and output as a gate
artifact. Exit zero approves the checkpoint; nonzero returns the process to
implementation until its configured attempt limit is reached.

## Concise agents and skills

Agents and skills are provider-neutral JSON data under `harness/agents/` and
`harness/skills/`. They contain short key/value arrays rather than long prompts.
The adapter decides how to render these fields for its model API.

Agent example:

```json
{
  "id": "verifier",
  "purpose": "Map gate evidence to acceptance criteria without editing files.",
  "reads": ["task", "specification", "gate_result", "repository"],
  "writes": ["verification_report"],
  "skills": ["verification"],
  "rules": ["Do not edit files.", "A check not observed is not checked."]
}
```

Definitions are JSON-Schema validated. Keep provider-specific prompts and tool
syntax in adapters, not in the core definitions.

## Explicit staging

The finalizer returns `changed_files`. The final stage copies those paths into a
staging manifest. Automatic `git add -- <path>...` is opt-in:

```toml
[git]
stage_on_success = true
```

Blanket staging is never used. Odin does not commit or push.

## Validate and test

```powershell
python odin.py validate
python -m unittest discover -s tests
```

`validate` checks every bundled workflow, agent, skill, and template, including
their cross-references and state transitions.

## Next work

The core deliberately stops at a command adapter boundary. Useful next steps:

- OpenAI-compatible HTTP adapter for Ollama, LM Studio, vLLM, and hosted APIs;
- benchmark runner that replays identical tasks across configured model
  profiles and records correctness, valid-contract rate, duration, and token
  usage;
- repository tool broker or container boundary that grants each agent only the
  capabilities declared by its definition;
- richer gate result contracts for per-test and per-acceptance-criterion
  evidence.
