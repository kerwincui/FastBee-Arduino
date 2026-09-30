"""Execute the production relay status route against a bounded Modbus device."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class ModbusRelayStatusTests(unittest.TestCase):
    def test_exact_quantity_range_validation_and_complete_reply(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        dependencies = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        self.assertTrue(dependencies, "Install project ArduinoJson dependency first")
        route = (ROOT / "src/network/handlers/ModbusRouteHandler.cpp").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="fastbee-relay-status-") as temp:
            temp = Path(temp)
            (temp / "relay_status.inc").write_text(
                definition(route, "void ModbusRouteHandler::handleModbusCoilStatus("), encoding="utf-8")
            executable = temp / ("status.exe" if os.name == "nt" else "status")
            built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    f"-I{temp}", f"-I{dependencies[0].parent}",
                                    str(ROOT / "test/unit/cpp/modbus_relay_status_harness.cpp"),
                                    "-o", str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            for case in ("exact", "bounds", "partial", "errors"):
                with self.subTest(case=case):
                    ran = subprocess.run([str(executable), case], capture_output=True, text=True, timeout=10)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == "__main__":
    unittest.main()
