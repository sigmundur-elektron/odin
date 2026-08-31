"""Drive the Python engine for one task so the C++ port can be diffed against it.

This is the reference half of the differential parity test in
tests/cpp/test_engine.cpp. Both engines are pointed at the same odin.toml and the
same task, and everything they write to disk is compared field by field once the
unavoidably volatile parts (ids, timestamps, timings) are normalised.

Prints the run directory it created, and nothing else, so the caller can parse it.

    python scripts/reference_run.py <odin.toml> <task.json>
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from harness.config import load_config  # noqa: E402
from harness.engine import WorkflowEngine  # noqa: E402
from harness.io import read_json  # noqa: E402


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    config_path = Path(sys.argv[1])
    task_path = Path(sys.argv[2])

    engine = WorkflowEngine(load_config(config_path))
    run_dir = engine.create_run(read_json(task_path), task_path)
    engine.run(run_dir)

    print(str(run_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
