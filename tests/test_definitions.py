from __future__ import annotations

import unittest

from harness.definitions import load_agent, load_skill, load_template, load_workflow


class DefinitionTests(unittest.TestCase):
    def test_builtin_definitions_validate(self) -> None:
        for identifier in ("analyst", "reproducer", "reviewer", "implementer", "verifier", "finalizer"):
            self.assertEqual(load_agent(identifier)["id"], identifier)
        for identifier in (
            "specification",
            "bug-reproduction",
            "review",
            "cpp-change",
            "verification",
            "finalization",
        ):
            self.assertEqual(load_skill(identifier)["id"], identifier)
        for identifier in ("feature", "bugfix"):
            self.assertEqual(load_workflow(identifier)["id"], identifier)
            self.assertEqual(load_template(identifier)["kind"], identifier)


if __name__ == "__main__":
    unittest.main()
