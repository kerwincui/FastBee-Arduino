"""Execute real relay admission, continuation, validation and debug snapshot code."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class ModbusRelayAsyncTests(unittest.TestCase):
    def test_admission_lifetime_and_debug_snapshot(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("C++ compiler required")
        route = (ROOT / "src/network/handlers/ModbusRouteHandler.cpp").read_text(encoding="utf-8")
        handler = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(encoding="utf-8")
        start = route.index("bool validRelayChannel(")
        end = route.index("\n}\n\nstruct ModbusRouteHandler::RelayJob", start)
        methods = route[start:end] + "\n"
        methods += definition(route, "struct ModbusRouteHandler::RelayJob") + ";\n"
        methods += "\n".join(definition(route, x) for x in (
            "void ModbusRouteHandler::relayTask(",
            "void ModbusRouteHandler::dispatchRelayRequest("))
        methods += "\n" + definition(handler, "bool ModbusHandler::getDebugSnapshot(")
        with tempfile.TemporaryDirectory(prefix="fastbee-relay-") as temp:
            temp = Path(temp)
            (temp / "relay_methods.inc").write_text(methods, encoding="utf-8")
            executable = temp / ("relay.exe" if os.name == "nt" else "relay")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     f"-I{temp}", str(ROOT / "test/unit/cpp/modbus_relay_async_harness.cpp"),
                                     "-o", str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_all_relay_routes_defer_and_toggle_reads_only_target(self):
        source = (ROOT / "src/network/handlers/ModbusRouteHandler.cpp").read_text(encoding="utf-8")
        setup = definition(source, "void ModbusRouteHandler::setupRoutes(")
        for suffix in ("Control", "Batch", "Delay", "Status"):
            self.assertIn("dispatchRelayRequest(request, &ModbusRouteHandler::handleModbusCoil" + suffix + ")", setup)
        control = definition(source, "void ModbusRouteHandler::handleModbusCoilControl(")
        self.assertIn("validRelayChannel(slaveInput, channelInput, baseInput)", control)
        self.assertIn("slaveAddr, 0x01, coilAddr, 1, true", control)
        self.assertNotIn("readR.data[channel]", control)
        self.assertEqual(control.count("readR.count < 1"), 2)


if __name__ == "__main__":
    unittest.main()
