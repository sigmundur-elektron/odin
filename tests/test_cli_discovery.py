from __future__ import annotations

import os
import tempfile
import unittest
from pathlib import Path

from harness.discovery import find_executable, probe_agent_cli


class FindExecutableTests(unittest.TestCase):
    def test_path_wins_and_is_reported_as_such(self) -> None:
        # Every platform this runs on has a shell interpreter on PATH.
        name = "cmd" if os.name == "nt" else "sh"
        located = find_executable(name)
        self.assertIsNotNone(located)
        self.assertEqual(located[1], "PATH")

    def test_finds_binary_in_an_extra_directory_off_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            suffix = ".cmd" if os.name == "nt" else ""
            binary = Path(directory) / f"totally-not-on-path{suffix}"
            binary.write_text("echo hi", encoding="utf-8")
            binary.chmod(0o755)
            located = find_executable("totally-not-on-path", (directory,))
            self.assertIsNotNone(located)
            self.assertEqual(Path(located[0]), binary)
            self.assertEqual(located[1], directory)

    def test_missing_executable_returns_none(self) -> None:
        self.assertIsNone(find_executable("odin-nonexistent-agent-cli-xyz"))

    def test_project_tools_are_found_outside_the_process_cwd(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            tools = project / ".odin" / "tools"
            tools.mkdir(parents=True)
            suffix = ".cmd" if os.name == "nt" else ""
            binary = tools / f"project-agent{suffix}"
            binary.write_text("echo hi", encoding="utf-8")
            binary.chmod(0o755)

            located = find_executable("project-agent", project_root=project)

            self.assertIsNotNone(located)
            self.assertEqual(Path(located[0]), binary)


class ProbeAgentCliTests(unittest.TestCase):
    def test_absent_cli_yields_no_provider(self) -> None:
        self.assertIsNone(
            probe_agent_cli("odin-nonexistent-agent-cli-xyz", ("--version",), deep=False)
        )

    def test_off_path_discovery_is_flagged_in_detail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            suffix = ".cmd" if os.name == "nt" else ""
            binary = Path(directory) / f"my-agent{suffix}"
            binary.write_text("echo hi", encoding="utf-8")
            binary.chmod(0o755)
            provider = probe_agent_cli("my-agent", ("--version",), False, (directory,))
            self.assertIsNotNone(provider)
            self.assertEqual(provider.status, "ready")
            self.assertIn("not on PATH", provider.detail)
            self.assertEqual(Path(provider.command), binary)


if __name__ == "__main__":
    unittest.main()
