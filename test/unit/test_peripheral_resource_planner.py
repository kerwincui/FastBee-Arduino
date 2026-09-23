"""Compile-and-run coverage for board profiles and resource preflight."""

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
HEADER = ROOT / "include/core/PeripheralResourcePlanner.h"
SOURCE = ROOT / "src/core/PeripheralResourcePlanner.cpp"
BOARD_HEADER = ROOT / "include/core/BoardProfile.h"
BOARD_SOURCE = ROOT / "src/core/BoardProfile.cpp"
HARNESS = ROOT / "test/unit/cpp/peripheral_resource_planner_harness.cpp"
MOCKS = ROOT / "test/unit/cpp/mocks"


class PeripheralResourcePlannerTests(unittest.TestCase):
    def test_public_contract_is_fixed_capacity_and_stably_coded(self):
        header = HEADER.read_text(encoding="utf-8")
        source = SOURCE.read_text(encoding="utf-8")
        board = BOARD_SOURCE.read_text(encoding="utf-8")

        self.assertIn("ResourceClaim claims[PREFLIGHT_MAX_CLAIMS]", header)
        self.assertIn("PreflightIssue issues[PREFLIGHT_MAX_ISSUES]", header)
        self.assertIn("preflightPeripheralChange", header)
        self.assertNotIn("std::vector", header + source)
        self.assertNotIn("std::map", header + source)
        for code in (
            "PIN_HARD_FORBIDDEN",
            "I2C_ADDRESS_CONFLICT",
            "SPI_CS_CONFLICT",
            "UART_PORT_EXHAUSTED",
            "LEDC_TIMER_CONFLICT",
            "RMT_CHANNEL_EXHAUSTED",
            "ADC_CHANNEL_CONFLICT",
            "ENCODER_SLOT_EXHAUSTED",
            "SDMMC_UNSUPPORTED",
        ):
            self.assertIn(f'return "{code}"', source)
        self.assertIn('"pz-esp32-v1.2"', board)
        self.assertIn('"Espressif ESP32-C6-DevKitC-1 User Guide', board)

    def test_focused_cpp_harness(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required for the focused planner harness")

        with tempfile.TemporaryDirectory(prefix="fastbee-planner-") as tmp:
            executable = pathlib.Path(tmp) / (
                "planner-tests.exe" if os.name == "nt" else "planner-tests"
            )
            command = [
                compiler,
                "-std=c++17",
                "-DUNIT_TEST",
                "-DARDUINOJSON_ENABLE_STD_STRING=1",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                f"-I{MOCKS}",
                f"-I{ROOT / 'test/mocks'}",
                f"-I{ROOT / 'include'}",
                str(HARNESS),
                str(BOARD_SOURCE),
                str(SOURCE),
                "-o",
                str(executable),
            ]
            compiled = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertEqual(
                compiled.returncode,
                0,
                f"Focused planner harness did not compile:\n{compiled.stdout}\n{compiled.stderr}",
            )
            executed = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
            self.assertEqual(
                executed.returncode,
                0,
                f"Focused planner harness failed:\n{executed.stdout}\n{executed.stderr}",
            )


if __name__ == "__main__":
    unittest.main()
