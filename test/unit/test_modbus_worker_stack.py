"""Regression for real COM15 float formatter and observation parse canaries."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class ModbusWorkerStackTests(unittest.TestCase):
    def compile_and_run(self, harness, snippets=None):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        json_header = next((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        with tempfile.TemporaryDirectory(prefix="fastbee-worker-stack-") as directory:
            temporary = Path(directory)
            if snippets:
                (temporary / "production_direct_dispatch.inc").write_text(snippets, encoding="utf-8")
            executable = temporary / "regression.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            command = [compiler, "-std=c++17", "-O2", "-fstack-usage", "-Wall", "-Wextra", "-Werror",
                       "-DFASTBEE_ENABLE_POINT_HISTORY=1", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                       "-DARDUINOJSON_ENABLE_STD_STRING=1", "-I" + str(ROOT / "test/mocks"),
                       "-I" + str(ROOT / "include"), "-I" + str(json_header.parent), "-I" + str(temporary),
                       str(ROOT / "test/unit/cpp" / harness), "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                    errors="replace", env=environment, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    env=environment, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            for usage in temporary.glob("*.su"):
                for line in usage.read_text().splitlines():
                    if "formatModbusFixed" in line or "publishObservation" in line or "handlePollArray" in line:
                        print("host stack (-O2; not device HWM): " + line)

    def test_fixed_format_matches_printf_without_newlib_formatting(self):
        self.compile_and_run("modbus_fixed_format_harness.cpp")

    def test_production_observation_dispatch_preserves_wire_and_quality_without_parse(self):
        manager = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
        scheduler = (ROOT / "src/core/PeriphExecScheduler.cpp").read_text(encoding="utf-8")
        snippets = definition(manager, "void PeriphExecManager::publishObservation(") + "\n"
        snippets += definition(scheduler, "void PeriphExecScheduler::handlePollArray(")
        self.compile_and_run("observation_direct_dispatch_harness.cpp", snippets)


if __name__ == "__main__":
    unittest.main()
