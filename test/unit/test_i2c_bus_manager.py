"""Run the real bus lease manager against concurrent recursive-mutex mocks."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class I2CBusManagerTests(unittest.TestCase):
    def test_real_manager_concurrency_and_leases(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required")
        with tempfile.TemporaryDirectory(prefix="fastbee-i2c-bus-") as tmp:
            executable = pathlib.Path(tmp) / ("bus-tests.exe" if os.name == "nt" else "bus-tests")
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                       f"-I{ROOT / 'test/unit/cpp/i2c_bus_mocks'}", f"-I{ROOT / 'test/mocks'}",
                       f"-I{ROOT / 'include'}", str(ROOT / "test/unit/cpp/i2c_bus_harness.cpp"),
                       str(ROOT / "src/core/I2CBusManager.cpp"), "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
