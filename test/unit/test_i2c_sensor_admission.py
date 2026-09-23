"""Exercise sensor reopen admission with the real bus manager and a held mutex."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SensorAdmissionTests(unittest.TestCase):
    def test_busy_reopen_keeps_old_lease_and_uses_only_100ms_admission(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Install the project's ArduinoJson dependency first")
        with tempfile.TemporaryDirectory(prefix="fastbee-sensor-admission-") as directory:
            executable = Path(directory) / ("admission.exe" if os.name == "nt" else "admission")
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                       f"-I{ROOT / 'test/unit/cpp/io_expander_provider_mocks'}",
                       f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       f"-I{json_headers[0].parent}",
                       str(ROOT / "test/unit/cpp/i2c_sensor_admission_harness.cpp"),
                       str(ROOT / "src/core/I2CBusManager.cpp"), "-o", str(executable)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
