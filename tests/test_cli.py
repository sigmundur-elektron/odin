from __future__ import annotations

import io
import unittest
from contextlib import redirect_stdout

from harness.cli import main


class CliTests(unittest.TestCase):
    def test_self_validation_does_not_require_project_config(self) -> None:
        output = io.StringIO()
        with redirect_stdout(output):
            code = main(["validate", "--self-only"])
        self.assertEqual(code, 0)
        self.assertIn('"status": "valid"', output.getvalue())


if __name__ == "__main__":
    unittest.main()
