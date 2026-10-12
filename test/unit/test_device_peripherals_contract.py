"""Check public peripheral examples against the real sensor config parser."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DevicePeripheralsContractTests(unittest.TestCase):
    def test_sensor_examples_roundtrip_and_reject_incompatible_changes(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        try:
            probe = subprocess.run([compiler, "--version"], capture_output=True,
                                   text=True, encoding="utf-8", errors="replace", timeout=10)
        except (OSError, subprocess.SubprocessError) as error:
            self.skipTest(f"C++ compiler is not runnable: {error}")
        if probe.returncode != 0:
            self.skipTest("C++ compiler is present but cannot be started in this environment")
        with tempfile.TemporaryDirectory(prefix="fastbee-device-peripherals-probe-") as probe_dir:
            probe_src = Path(probe_dir) / "probe.cpp"
            probe_bin = Path(probe_dir) / ("probe.exe" if os.name == "nt" else "probe")
            probe_src.write_text("int main() { return 0; }\n", encoding="utf-8")
            link_probe = subprocess.run([compiler, str(probe_src), "-o", str(probe_bin)],
                                        capture_output=True, text=True,
                                        encoding="utf-8", errors="replace", timeout=20)
            if link_probe.returncode != 0 or not probe_bin.exists():
                self.skipTest("C++ compiler cannot link a runnable test binary in this environment")
        headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not headers:
            self.skipTest("Install the project's ArduinoJson dependency first")
        with tempfile.TemporaryDirectory(prefix="fastbee-device-peripherals-") as directory:
            executable = Path(directory) / ("contract.exe" if os.name == "nt" else "contract")
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                       "-DARDUINOJSON_ENABLE_STD_STRING=1",
                       f"-I{ROOT / 'test/unit/cpp/sensor_mocks'}",
                       f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       f"-I{headers[0].parent}",
                       str(ROOT / "test/unit/cpp/device_peripherals_contract_harness.cpp"),
                       str(ROOT / "src/core/SensorConfigSupport.cpp"),
                       str(ROOT / "src/peripherals/drivers/INA226Driver.cpp"),
                       "-o", str(executable)]
            build = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                   encoding="utf-8", errors="replace", timeout=90)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], cwd=ROOT, capture_output=True,
                                    text=True, encoding="utf-8", errors="replace", timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("device peripheral contract checks passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
