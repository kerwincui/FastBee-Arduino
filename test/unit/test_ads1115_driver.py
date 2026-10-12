"""Compile actual ADS driver, configuration, cache and I2C manager offline."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
DEPENDENCIES = Path(os.environ.get("FASTBEE_DEPENDENCY_ROOT", ROOT))

class ADS1115DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        headers = sorted((DEPENDENCIES / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not compiler or not headers:
            raise RuntimeError("C++17 compiler and ArduinoJson headers required")
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-ads-driver-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / ("ads.exe" if os.name == "nt" else "ads")
        sources = ["test/unit/cpp/ads1115_driver_harness.cpp", "src/core/I2CBusManager.cpp",
            "src/core/SensorConfigSupport.cpp", "src/core/SensorDriverCache.cpp",
            "src/peripherals/drivers/ADS1115Driver.cpp", "src/peripherals/drivers/INA226Driver.cpp"]
        cls.inputs = [ROOT / source for source in sources] + [ROOT / "test/unit/cpp/ads1115_scan_cases.inc"]
        cls.inputs += list((ROOT / "test/unit/cpp/ads1115_driver_mocks").rglob("*.h"))
        cls.inputs += list((ROOT / "include/peripherals/drivers").glob("ADS1115*.h"))
        cls.inputs += [ROOT / path for path in (
            "include/core/interfaces/ISensorDriver.h", "include/core/SensorConfigSupport.h",
            "include/core/SensorDriverCache.h", "include/core/SensorSampleMetadata.h",
            "include/core/SensorDriverMetadata.h", "include/core/PeripheralConfig.h",
            "include/core/I2CBusManager.h")]
        cls.inputs += [ROOT / "docs/ads1115-example-20261002.json",
                       ROOT / "docs/ads1115-scan-example-20261004.json"]
        cls.before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            f"-I{ROOT / 'test/unit/cpp/ads1115_driver_mocks'}", f"-I{ROOT / 'test/mocks'}",
            f"-I{ROOT / 'include'}", f"-I{headers[0].parent}"]
        command += [str(ROOT / source) for source in sources] + ["-o", str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        if os.environ.get("FASTBEE_ADS_METADATA_OUT"):
            metadata = subprocess.run([str(cls.exe), "metadata"], capture_output=True, text=True, timeout=10)
            if metadata.returncode:
                raise AssertionError(metadata.stdout + metadata.stderr)
            parsed = json.loads(metadata.stdout)
            with Path(os.environ["FASTBEE_ADS_METADATA_OUT"]).open("x", encoding="utf-8") as stream:
                json.dump(parsed, stream, indent=2)

    @classmethod
    def tearDownClass(cls):
        if cls.before != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError("Inputs changed while ADS driver tests ran")

    def check_mode(self, mode):
        args = [str(self.exe), mode]
        if mode == "example": args.append(str(ROOT / "docs/ads1115-example-20261002.json"))
        if mode == "scan_example": args.append(str(ROOT / "docs/ads1115-scan-example-20261004.json"))
        result = subprocess.run(args, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"ADS driver {mode} passed", result.stdout)

for mode in ("registration", "example", "config", "schema_source", "all_configs", "lost_lease", "invalid", "disabled", "cache", "duplicate", "faults",
             "success", "continuous", "wrap", "negative", "rail", "timeout", "missed", "lost",
             "short", "negative_read", "short_write", "closed", "calibration",
             "scan_config", "scan_masks", "scan_values", "scan_cache", "scan_faults", "scan_fault_cache",
             "scan_release", "scan_replace", "scan_epoch", "scan_parallel", "scan_lock_timeout",
             "scan_outer_timeout_after_success", "scan_lock_timeout_after_success",
             "scan_wrap", "scan_timeout", "scan_lease", "scan_cancel_all", "scan_step_bound", "scan_capacity", "scan_protocol_failures", "scan_example"):
    setattr(ADS1115DriverTests, f"test_{mode}", lambda self, mode=mode: self.check_mode(mode))

if __name__ == "__main__":
    unittest.main()
