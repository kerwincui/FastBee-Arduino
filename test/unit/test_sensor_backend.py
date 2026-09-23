"""Compile and execute the actual sensor cache, registry, and INA226 driver."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SensorBackendTests(unittest.TestCase):
    def test_real_cpp_sensor_backend(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        json_candidates = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_candidates:
            self.skipTest("Install the project's ArduinoJson dependency first")
        with tempfile.TemporaryDirectory(prefix="fastbee-sensor-") as directory:
            executable = Path(directory) / ("sensor.exe" if os.name == "nt" else "sensor")
            command = [compiler, "-std=c++17", "-pthread", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                       "-DARDUINOJSON_ENABLE_STD_STRING=1", "-Wall", "-Wextra",
                       f"-I{ROOT / 'test/unit/cpp/sensor_mocks'}",
                       f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       f"-I{json_candidates[0].parent}",
                       str(ROOT / "test/unit/cpp/sensor_backend_harness.cpp"),
                       str(ROOT / "src/core/SensorDriverCache.cpp"),
                       str(ROOT / "src/core/SensorConfigSupport.cpp"),
                       str(ROOT / "src/peripherals/drivers/INA226Driver.cpp"),
                       str(ROOT / "src/peripherals/drivers/AHT20Driver.cpp"),
                       str(ROOT / "src/peripherals/drivers/BH1750Driver.cpp"),
                       str(ROOT / "src/peripherals/drivers/SHT31Driver.cpp"),
                       str(ROOT / "src/peripherals/drivers/BMP280Driver.cpp"),
                       str(ROOT / "src/peripherals/drivers/MPU6050Driver.cpp"),
                       "-o", str(executable)]
            build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("sensor backend behavior checks passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
