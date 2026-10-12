"""Compile the actual H1 state machine; no production driver is registered."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TEST_ROOT = Path(__file__).resolve().parent


class ADS1115SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("C++17 compiler required")
        override = ROOT / "include"
        cls.inputs = [override / "peripherals/drivers/ADS1115ConversionSession.h",
                      ROOT / "include/peripherals/drivers/ADS1115Contract.h",
                      TEST_ROOT / "cpp/ads1115_session_harness.cpp"]
        cls.before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.directory = tempfile.TemporaryDirectory(prefix="fastbee-ads1115-session-")
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name) / ("session.exe" if os.name == "nt" else "session")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            f"-I{override}", f"-I{ROOT / 'include'}",
            str(TEST_ROOT / "cpp/ads1115_session_harness.cpp"),
            "-o", str(cls.executable)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs] != cls.before:
            raise AssertionError("Session production/harness inputs changed during test")

    def check_mode(self, mode):
        result = subprocess.run([str(self.executable), mode], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(mode + " PASS", result.stdout)


for _mode in ("all_configs", "drain_single", "drain_continuous", "stale_idle",
              "config_loss", "cancel_generation", "busy_begin", "timeout_phases",
              "timer_wrap", "transport_failures", "write_nack", "invalid_config",
              "rails_and_restart", "out_of_order"):
    setattr(ADS1115SessionTests, f"test_{_mode}",
            lambda self, mode=_mode: self.check_mode(mode))


if __name__ == "__main__":
    unittest.main()
