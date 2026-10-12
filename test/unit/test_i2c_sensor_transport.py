"""Exercise the actual I2CSensorBus and manager; never open a hardware bus."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
TEST_ROOT = Path(__file__).resolve().parent


class I2CSensorTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("A C++17 compiler is required")
        json_headers = sorted((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            raise RuntimeError("Install the project's ArduinoJson dependency first")
        override = ROOT / "include"
        cls.inputs = [override / "peripherals/drivers/I2CSensorBus.h",
                      ROOT / "include/core/I2CBusManager.h", ROOT / "src/core/I2CBusManager.cpp",
                      TEST_ROOT / "cpp/i2c_sensor_transport_harness.cpp",
                      TEST_ROOT / "cpp/i2c_sensor_transport_mocks/Wire.h"]
        cls.before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-sensor-transport-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / ("transport.exe" if os.name == "nt" else "transport")
        command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                   f"-I{TEST_ROOT / 'cpp/i2c_sensor_transport_mocks'}",
                   f"-I{ROOT / 'test/unit/cpp/io_expander_provider_mocks'}",
                   f"-I{ROOT / 'test/mocks'}", f"-I{override}", f"-I{ROOT / 'include'}",
                   f"-I{json_headers[0].parent}",
                   str(TEST_ROOT / "cpp/i2c_sensor_transport_harness.cpp"),
                   str(ROOT / "src/core/I2CBusManager.cpp"), "-o", str(cls.executable)]
        build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                               encoding="utf-8", errors="replace", timeout=90)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        after = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        if after != cls.before:
            raise AssertionError("Production or harness inputs changed during the test")

    def check_mode(self, mode):
        result = subprocess.run([str(self.executable), mode], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"transport {mode} passed", result.stdout)


for _mode in ("pointer_short", "pointer_nack", "read_missing", "read_negative",
              "short_count", "bounded_drain", "drain_negative", "null_read",
              "zero_read", "oversized_read", "null_write", "probe", "write_short",
              "success", "length_boundary", "all_codes", "recovery", "busy",
              "closed", "peer_survives"):
    setattr(I2CSensorTransportTests, f"test_{_mode}",
            lambda self, mode=_mode: self.check_mode(mode))


if __name__ == "__main__":
    unittest.main()
