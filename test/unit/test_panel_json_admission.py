"""Compile and exercise the production PanelJsonHandler admission wrapper."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
ROUTE = ROOT / "src/network/handlers/ScreenPanelRouteHandler.cpp"
HARNESS = ROOT / "test/unit/cpp/panel_json_admission_harness.cpp"


def _braced(source: str, opening: int) -> str:
    depth = 0
    tokens = re.compile(
        r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]'
    )
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[opening : token.end()]
    raise AssertionError("Unterminated PanelJsonHandler class")


def _production_handler() -> str:
    source = ROUTE.read_text(encoding="utf-8")
    marker = "class PanelJsonHandler final : public AsyncWebHandler"
    start = source.index(marker)
    opening = source.index("{", start)
    definition = source[start:opening] + _braced(source, opening)
    semicolon = source.find(";", start + len(definition))
    if semicolon < 0 or source[start + len(definition) : semicolon].strip():
        raise AssertionError("PanelJsonHandler class terminator is missing")
    return definition + ";"


class PanelJsonAdmissionTests(unittest.TestCase):
    def test_production_handler_rejects_before_delegate_allocation_and_parse(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(
            compiler, "A C++17 compiler is required for PanelJsonHandler checks"
        )

        with tempfile.TemporaryDirectory(prefix="fastbee-panel-json-") as directory:
            temporary = Path(directory)
            (temporary / "panel_json_handler_under_test.inc").write_text(
                _production_handler(), encoding="utf-8"
            )
            executable = temporary / (
                "panel-json-admission.exe" if os.name == "nt" else "panel-json-admission"
            )
            command = [
                compiler,
                "-std=c++17",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{temporary}",
                str(HARNESS),
                "-o",
                str(executable),
            ]
            built = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=60,
            )
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)

            ran = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
            )
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("panel JSON admission checks passed", ran.stdout)


if __name__ == "__main__":
    unittest.main()
