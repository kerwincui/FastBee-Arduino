"""Run the production bounded grouping/execution and panel source collector."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class SensorGroupExecutionTests(unittest.TestCase):
    def test_production_group_boundaries_failures_and_panel_sources(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        json = ROOT / ".pio/libdeps/esp32s3-F16R8/ArduinoJson/src"
        executor = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")
        panel = (ROOT / "src/core/ScreenPanelManager.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/core/PeriphExecManager.h").read_text(encoding="utf-8")
        methods = "\n".join([
            definition(header, "enum class ObservationQuality") + ";",
            definition(header, "struct ObservationUpdate") + ";",
            definition(header, "struct ObservationSnapshot") + ";",
            definition(executor, "struct PreparedSensorRead") + ";",
        ])
        functions = "\n".join([
            definition(executor, "size_t sensorReadGroupSize("),
            definition(executor, "bool PeriphExecExecutor::executeSensorReadGroup("),
            definition(panel, "std::vector<ObservationSnapshot> panelObservationSnapshots("),
        ])
        with tempfile.TemporaryDirectory(prefix="fastbee-sensor-group-") as directory:
            target = Path(directory)
            (target / "sensor_group_types.inc").write_text(methods, encoding="utf-8")
            (target / "sensor_group_methods.inc").write_text(functions, encoding="utf-8")
            executable = target / "group.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run([compiler, "-std=c++17", "-O2", "-fstack-usage", "-Wall", "-Wextra", "-Werror",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                f"-I{target}", f"-I{json}",
                str(ROOT / "test/unit/cpp/sensor_group_execution_harness.cpp"), "-o", str(executable)],
                capture_output=True, text=True, timeout=60, env=environment)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())
            for usage in target.glob("*.su"):
                for line in usage.read_text().splitlines():
                    if any(name in line for name in ("executeSensorReadGroup", "sensorReadGroupSize", "panelObservationSnapshots")):
                        print("host stack (-O2, mocked hardware): " + line)
            broken = functions.replace('initial.targetPeriphId != (parameters["periphId"] | "")',
                '(parameters["periphId"] | "") != initial.targetPeriphId')
            self.assertNotEqual(broken, functions)
            (target / "sensor_group_methods.inc").write_text(broken, encoding="utf-8")
            invalid = subprocess.run([compiler, "-std=c++17", "-fsyntax-only",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                f"-I{target}", f"-I{json}", str(ROOT / "test/unit/cpp/sensor_group_execution_harness.cpp")],
                capture_output=True, text=True, timeout=60, env=environment)
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn("deleted", invalid.stderr)
        snapshot = definition(panel, "bool ScreenPanelManager::getSnapshotImpl(")
        self.assertEqual(snapshot.count("panelObservationSnapshots("), 1)
        self.assertNotIn(".getObservation(", snapshot)
        action = definition(executor, "bool PeriphExecExecutor::executeSensorReadAction(")
        self.assertLess(action.index("if (prepared)"), action.index("updateSensorReadCache("))
        self.assertIn("value.sampleTimestamp = sensorSampleTimestamp", action)
        self.assertIn("value.maxAgeMs = sensorMaxAgeMs", action)


if __name__ == "__main__":
    unittest.main()
