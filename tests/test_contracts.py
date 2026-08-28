from __future__ import annotations

import unittest

from harness.contracts import validate
from harness.errors import ContractError


class ContractTests(unittest.TestCase):
    def test_feature_task_is_valid(self) -> None:
        validate(
            {"id": "feature-1", "kind": "feature", "title": "Feature", "request": "Add it."},
            "task",
        )

    def test_bugfix_requires_reproduction(self) -> None:
        with self.assertRaisesRegex(ContractError, "reproduction"):
            validate(
                {"id": "bug-1", "kind": "bugfix", "title": "Bug", "request": "Fix it."},
                "task",
            )

    def test_handoff_rejects_unstructured_status(self) -> None:
        with self.assertRaises(ContractError):
            validate(
                {"status": "looks-good", "summary": "ok", "artifacts": {}, "findings": []},
                "handoff",
            )


if __name__ == "__main__":
    unittest.main()
