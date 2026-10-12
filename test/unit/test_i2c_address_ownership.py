"""Exercise address exclusion with real bus admission and timed mutexes."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DEPENDENCIES = Path(os.environ.get("FASTBEE_DEPENDENCY_ROOT", ROOT))

class I2CAddressOwnershipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("C++17 compiler required")
        headers = sorted((DEPENDENCIES / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not headers:
            raise RuntimeError("ArduinoJson headers required")
        cls.inputs = [ROOT / p for p in ("include/core/I2CBusManager.h",
            "src/core/I2CBusManager.cpp", "include/peripherals/drivers/I2CSensorBus.h",
            "test/unit/cpp/i2c_address_ownership_harness.cpp")]
        cls.before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-address-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / ("ownership.exe" if os.name == "nt" else "ownership")
        command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            f"-I{ROOT / 'test/unit/cpp/i2c_sensor_transport_mocks'}",
            f"-I{ROOT / 'test/unit/cpp/io_expander_provider_mocks'}",
            f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}", f"-I{headers[0].parent}",
            str(ROOT / "test/unit/cpp/i2c_address_ownership_harness.cpp"),
            str(ROOT / "src/core/I2CBusManager.cpp"),
            str(ROOT / "src/peripherals/IoExpanderProvider.cpp"),
            str(ROOT / "src/peripherals/IoExpander.cpp"), "-o", str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.before != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError("Test inputs changed while running")

    def check_mode(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"ownership {mode} passed", result.stdout)

for mode in ("duplicate", "release", "alias", "different", "concurrent",
             "claim_rules", "retire", "recovery", "capacity", "provider_collision"):
    setattr(I2CAddressOwnershipTests, f"test_{mode}", lambda self, mode=mode: self.check_mode(mode))

if __name__ == "__main__":
    unittest.main()
