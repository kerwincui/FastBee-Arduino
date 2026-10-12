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
        self.assertIn("validRelayChannel(slave, channel, base)", control)
        self.assertIn("snapshotOrdinaryRelayEndpoint(", control)
        self.assertLess(control.index("snapshotOrdinaryRelayEndpoint("),
                        control.index("beginHttpModbusRelayOutput("))
        self.assertIn("*modbus, target, function, address, value, control, result, 0, toggle", control)
        self.assertNotIn("readRegistersOnce", control)
        self.assertNotIn("writeCoilOnce", control)
        self.assertNotIn("writeRegisterOnce", control)
        adapter = (ROOT / "src/protocols/ModbusOrdinaryControl.cpp").read_text(encoding="utf-8")
        write = definition(adapter, "OneShotResult ModbusHandler::writeOrdinaryRelayOnce(")
        # Toggle reads one exact endpoint and writes it under the same bus guard.
        # Actual RTU success, malformed read and retirement between read/write
        # are exercised by test_modbus_ordinary_control.
        self.assertEqual(write.count("OrdinaryBusGuard lock(_oneShotSemaphore)"), 1)
        self.assertLess(write.index("OrdinaryBusGuard lock("), write.index("if (toggle)"))
        self.assertIn("static_cast<uint8_t>(function == 5 ? 1 : 3)", write)
        self.assertIn("static_cast<uint8_t>(address), 0, 1, 0, 0", write)
        self.assertIn("result.count != (coil ? 6 : 7)", write)
        self.assertIn("result.data[2] != (coil ? 1 : 2)", write)
        self.assertLess(write.index("CONTROL_TOGGLE_READ_UNCONFIRMED"),
                        write.index("_ordinaryWriteTicket = &ticket"))
        # The shared buffer also supports an 11-byte single-word FC16 motor
        # write. Ordinary FC05/06 relay writes still transmit only eight bytes.
        self.assertIn("const size_t length = function == 16 ? 11 : 8", write)
        self.assertIn("request, length, target.slave, timeout, 0, true", write)


if __name__ == "__main__":
    unittest.main()
