"""Check the actual CLI in a feature-disabled SDK build."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


class DisabledExecutionCLI(unittest.TestCase):
    executable: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        if cls.executable is None:
            raise unittest.SkipTest("a feature-disabled CLI path is required")

    def check_environment(self, environment: dict[str, object]) -> None:
        with tempfile.TemporaryDirectory(prefix="neverd-disabled-") as directory:
            configuration = Path(directory) / "execution.json"
            configuration.write_text(
                json.dumps({"environment": environment}), encoding="utf-8"
            )
            result = subprocess.run(
                [str(self.executable), "execution", str(configuration)],
                input='{"id":"run","op":"run"}\n',
                capture_output=True,
                text=True,
                timeout=15,
            )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("disabled", result.stderr.lower())
        self.assertEqual(result.stdout, "")

    def test_buffer_startup_reports_disabled_execution(self) -> None:
        self.check_environment({"kind": "buffer"})

    def test_driver_startup_reports_disabled_before_opening_the_image(self) -> None:
        self.check_environment(
            {"kind": "windows-driver", "image": "not-provided.sys"}
        )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--neverd", type=Path, required=True)
    options, remaining = parser.parse_known_args()
    DisabledExecutionCLI.executable = options.neverd.resolve(strict=True)
    unittest.main(argv=[__file__, *remaining])
