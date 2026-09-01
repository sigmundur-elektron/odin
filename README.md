# Odin

Odin is a framework-neutral workflow harness for C++ projects. It does not
depend on opencode, GitHub Copilot, a particular model provider, CMake, or a
specific test framework. It coordinates work through JSON contracts and runs
the commands supplied by the consuming project.

## Build and install

Odin's primary interface is the native `odin` executable. Python 3.11+ with
`jsonschema` remains a runtime dependency for schema validation, provider
discovery, credentials, and adapters.

```powershell
python -m pip install -r scripts/requirements.txt
cmake -S . -B build-cpp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cpp
cmake --install build-cpp --prefix out/odin
```

The install contains the executable under `bin/` and its Python runtime,
definitions, schemas, and bundled adapters under `share/odin/`. Odin resolves
those assets from the executable independently of the consuming project. Set
`ODIN_PYTHON` when the desired interpreter is not available as `python` on
`PATH`.

During source-tree development, use `build-cpp/odin.exe` on Windows or
`build-cpp/odin` on POSIX. The examples below use `odin` for either an installed
executable or that build artifact.

## Start one complete process

Feature:

```powershell
odin start examples/feature.json
```

Bug fix:

```powershell
odin start examples/bugfix.json
```

The same command creates a durable run, executes its state machine, follows
revision transitions when a checkpoint fails, and stops at `complete` or
`blocked`. Resume an interrupted run with:

```powershell
odin resume <run-id>
```

Inspect a run without changing it:

```powershell
odin status <run-id>
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
events/*.json   per-stage handoffs
journal/*.json  immutable stage-started and stage-completed records
run.lock        OS-backed per-run execution lock
```

State files use atomic replacement, so readers do not observe partial JSON.
Each new stage attempt is journaled and persisted before external work begins.
If a process stops after an attempt starts but before completion is committed,
plain resume blocks with an uncertain outcome rather than silently replaying the
stage. Acknowledging a possible duplicate external effect is explicit:

```powershell
odin resume <run-id> --retry-interrupted
```

See `docs/state-format.html` for the state v2 commit and recovery protocol.

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

## Linking models

Odin bundles no provider and hardcodes no model id. Instead it discovers what
this machine can actually reach:

```powershell
odin doctor                      # what is reachable right now
odin doctor --deep               # also enumerate agent CLI models
odin doctor --emit-config        # paste-ready odin.toml blocks
odin doctor --json               # machine-readable, for the GUI
```

`doctor` probes, with short timeouts and no dependencies:

| Transport | Covers | Probe |
|---|---|---|
| OpenAI-compatible HTTP | Ollama, LM Studio, llama.cpp, vLLM, LiteLLM, OpenAI, Azure, OpenRouter, Groq, Together, DeepSeek | `GET /v1/models` |
| Ollama native | local models plus parameter size and quantization | `GET /api/tags` |
| Agent CLI | OpenCode, Claude Code, Aider, Codex, `llm` | binary on `PATH` |
| Hosted key | any provider whose credential is exported | environment variable |

Because nearly every provider speaks the same HTTP shape, two adapters cover the
ecosystem:

- `adapters/openai_compatible.py` — any OpenAI-compatible endpoint, local or hosted
- `adapters/cli_agent.py` — any command-line agent, including streaming JSON output

Adding a provider is configuration, not code. `--emit-config` writes the blocks
for you from what it observed, so model ids are never guessed:

```toml
[adapters.http-ollama]
command = [
  "python", "adapters/openai_compatible.py",
  "--model", "{model}",
  "--base-url", "http://127.0.0.1:11434/v1"
]
timeout_seconds = 900

[models.qwen2-5-coder-32b]
adapter = "http-ollama"
model = "qwen2.5-coder:32b"
parameter_billions = 32.8
tags = ["ollama", "discovered"]
```

Project children receive a small operational environment rather than every
variable exported by the shell that launched Odin. `PATH`, home/temp, locale,
certificate paths, and required Windows process variables are retained.
Adapters and gates that need another inherited value name it explicitly:

```toml
[adapters.hosted]
command = [
  "python", "adapters/openai_compatible.py",
  "--model", "{model}",
  "--base-url", "https://openrouter.ai/api/v1",
  "--api-key-env", "OPENROUTER_API_KEY"
]
inherit_environment = ["OPENROUTER_API_KEY"]
environment = { PYTHONUTF8 = "1" }
```

Prefer stored credentials when possible; they require no inherited secret.
Values explicitly imported for a child, and credentials resolved by bundled
adapters, are redacted from captured stdout/stderr before durable state is
written. This protects accidental echoes; arbitrary same-user child code can
still read a project-local credential store, so full isolation requires the
planned credential broker or sandbox boundary.

Credentials are stored by Odin and referenced by **name**:

```toml
command = [
  "python", "adapters/openai_compatible.py",
  "--model", "{model}",
  "--base-url", "https://openrouter.ai/api/v1",
  "--credential", "openrouter"
]
```

Nothing secret enters the repository or `odin.toml`, and the same configuration
works unchanged inside a container.

## Credentials

Agents and subagents need model access at run time, so Odin stores provider
credentials itself rather than depending on whichever vendor CLI is installed.

```powershell
odin auth set openrouter        # prompts, input not echoed
odin auth set openai --stdin    # read from a pipe, for scripts
odin auth list                  # values masked
odin auth remove openrouter
```

Already authenticated somewhere else? Import an existing token instead of
logging in again:

```powershell
odin auth import `
  --from-file "$env:USERPROFILE\.local\share\opencode\auth.json" `
  --provider github-copilot
```

The store lives at `.odin/credentials.json`, inside `.odin/`, which is
gitignored. Design properties, each covered by a test:

| Property | How |
|---|---|
| Secrets never reach `argv` | Adapters receive `--credential <name>` and read the value themselves; process listings are world-readable on most systems |
| Secrets never reach output | `auth list`, `doctor`, and all reports render through `mask()`, e.g. `sk-...cdef` |
| Owner-only file mode | `0600` on POSIX; on Windows the store inherits the user-profile ACL |
| Environment beats disk | `--api-key-env`, then `ODIN_CREDENTIAL_<NAME>`, then the store, so CI and containers inject without writing to disk |
| Expired tokens fail loudly | Imported OAuth entries record `expires` and are refused with a clear message rather than sent and rejected |

## Optional tooling

Odin installs nothing on its own. If you want an agent CLI, ask for it
explicitly:

```powershell
odin tools list
odin tools install opencode
```

Tools land in `.odin/tools/<name>/`, which is gitignored and already searched by
`doctor`, so nothing appears in your repository root or in `git status`. An
installed OpenCode CLI reuses an existing OpenCode login, so no second
authentication step is needed.

Discovery also searches beyond `PATH` — npm global prefixes, Scoop and WinGet
shims, `~/.local/bin`, `%LOCALAPPDATA%\Programs`, and project `node_modules/.bin`
— and reports tools it finds off-`PATH` with their absolute path. Point it
somewhere else with:

```powershell
odin doctor --deep --path <directory-containing-the-binary>
```


### Model output is recovered, not trusted

Smaller models wrap JSON in prose, markdown fences, or reasoning preamble. Both
adapters share `harness/extract.py`, which recovers the object from fenced
blocks, trailing commentary, restated examples, and streamed event deltas. If
recovery fails, the adapter exits nonzero and the stage is recorded as `blocked`
rather than silently corrupting the run. The engine then re-validates against
`handoff.schema.json`, so a malformed reply cannot enter the artifact history.

### Choosing sizes empirically

Model size is descriptive metadata, not a threshold. Use `--model` to run the
same workflow against a different profile, or compare profiles directly:

```powershell
odin benchmark examples/feature.json --models coder-32b coder-80b frontier
```

Each profile gets a fresh run directory, but profiles currently share one
mutable project workspace and run sequentially. Treat results as diagnostic,
not fair comparative evidence, until per-profile workspace isolation is added.
Odin records completion status, transitions, wall-clock duration, and model
metadata under `.odin/benchmarks/`.

Inspect what is currently configured:

```powershell
odin models
odin models --json
```

The checked-in `mock` adapter is deterministic test infrastructure. It proves
workflow mechanics and contracts; it is not an implementation model.

## Driving Odin from a GUI

Odin is designed to be a backend for a front-end, not only a CLI. Core query and
run commands emit JSON, and run state is durable on disk, so the planned GUI can
wrap the CLI without linking to its C++ ABI:

```text
.odin/runs/<run-id>/
    task.json         immutable validated request
    state.json        current stage, status, attempts, transitions
    context.json      artifacts and full history
    events/NNN-*.json per-stage handoffs, written atomically
```

A C++/ImGui front-end is planned as a higher-level wrapper around the CLI, in
the same way a Git GUI adds workflows without replacing Git. It integrates by:

1. calling `odin doctor --json` to populate a model picker with providers
   that are genuinely reachable;
2. calling `odin models --json` to show configured profiles and routing;
3. spawning `odin start <task.json>` as a child process;
4. watching `.odin/runs/<id>/events/` and re-reading `state.json` to render
   stage progress, checkpoint verdicts, and loop-backs live;
5. calling `odin resume <run-id>` after an interruption.

Because files are written with atomic replacement, a reader never observes a
half-written state. A GUI can poll or use a filesystem watcher without locking.


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

The implementer returns `changed_files`. The final stage copies those paths into
a staging manifest. Automatic `git add -- <path>...` is opt-in:

```toml
[git]
stage_on_success = true
timeout_seconds = 300
```

`changed_files` must contain unique, normalized project-relative paths using `/`.
Absolute paths, traversal, directories, duplicates, malformed values, and paths
that resolve outside the project root are blocked. Automatic staging requires
the project root to be the Git worktree root and uses literal pathspecs. Blanket
staging is never used. Odin verifies the exact changed-file and index sets before
reporting `staged_files`. Odin does not commit or push.

## Validate and test

```powershell
odin validate
python -m unittest discover -s tests
cmake --build build-cpp
python scripts/gate.py --require-native
```

`validate` checks every bundled workflow, agent, skill, and template, including
their cross-references and state transitions.

## Next work

The core stops at the adapter boundary and the filesystem event log. Useful
next steps:

- **Capability-based routing.** Declare what a role *needs*
  (`min_context`, `tools`, `prefer = ["local"]`) and let Odin resolve it against
  discovered models, with automatic fallback when the preferred one is offline.
  Today routing names a profile directly.
- **Contract health probing.** `doctor --probe` would send each discovered model
  a tiny handoff-shaped request and record reachability, valid-JSON rate, and
  latency into `.odin/health.json`. That answers "is this model good enough for
  the verifier role?" for pennies, before committing a full run.
- **Token and cost metrics** in adapter metadata, so `benchmark` compares spend
  as well as duration.
- **`odin serve`**, a thin localhost control plane, if the GUI outgrows spawning
  the CLI and watching the run directory.
- **Tool broker / container boundary** granting each agent only the capabilities
  its definition declares.
- **Richer gate contracts** carrying per-test and per-acceptance-criterion
  evidence rather than a single exit code.
