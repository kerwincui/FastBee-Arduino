"""Compile the actual PCF8574 state machine and exercise bus-failure semantics."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class IoExpanderTests(unittest.TestCase):
    def test_real_provider_does_not_wait_or_touch_wire_when_bus_is_busy(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required")
        with tempfile.TemporaryDirectory(prefix="fastbee-io-provider-") as tmp:
            executable = pathlib.Path(tmp) / ("provider.exe" if os.name == "nt" else "provider")
            result = subprocess.run([
                compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'test/unit/cpp/io_expander_provider_mocks'}",
                f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                str(ROOT / "test/unit/cpp/io_expander_provider_harness.cpp"),
                str(ROOT / "src/peripherals/IoExpander.cpp"),
                str(ROOT / "src/peripherals/IoExpanderProvider.cpp"),
                str(ROOT / "src/core/I2CBusManager.cpp"), "-o", str(executable),
            ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_driver_state_machine(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required")
        with tempfile.TemporaryDirectory(prefix="fastbee-io-expander-") as tmp:
            executable = pathlib.Path(tmp) / ("io-tests.exe" if os.name == "nt" else "io-tests")
            result = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{ROOT / 'include'}", str(ROOT / "test/unit/cpp/io_expander_harness.cpp"),
                str(ROOT / "src/peripherals/IoExpander.cpp"), "-o", str(executable),
            ], capture_output=True, text=True, encoding="utf-8", errors="replace")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
