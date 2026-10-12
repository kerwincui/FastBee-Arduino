"""Real Modbus notification/statistics bodies versus frozen original at all OOM budgets."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(os.environ.get("FASTBEE_TEST_ROOT", next(
    path for path in Path(__file__).resolve().parents if (path / "platformio.ini").is_file())))
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
HERE = Path(__file__).resolve().parent
CPP = HERE if (HERE / "modbus_status_fields_harness.cpp").is_file() else ROOT / "test/unit/cpp"
sys.path.insert(0, str(ROOT / "test/unit"))
from test_sensor_publication import definition

SIGNATURES = (("void ModbusHandler::_notifyStatusChange(", 8),
              ("String ModbusHandler::getPollStatistics(", 24))


def declaration(source, name):
    matches = re.findall(r"^\s*[^\n;{}]+?\b" + re.escape(name) + r"\s*;", source, re.M)
    if len(matches) != 1:
        raise AssertionError("Production field declaration seam: " + name)
    return matches[0].strip() + "\n"


class ModbusStatusFieldsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE",
                       SUPPORT / ".pio/libdeps/native/ArduinoJson/src"))
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson required")
        cls.inputs = [Path(__file__), CPP / "modbus_status_fields_harness.cpp",
            CPP / "modbus_status_fields_oracle.inc", ROOT / "test/unit/cpp/serializer_trace_allocator.h",
            ROOT / "test/unit/test_sensor_publication.py", SUPPORT / "test/mocks/Arduino.h",
            ROOT / "include/protocols/ModbusHandler.h", ROOT / "src/protocols/ModbusHandler.cpp",
            ROOT / "include/core/SystemConstants.h", ROOT / "include/network/FixedJsonField.h",
            ROOT / "src/network/FixedJsonField.cpp"]
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.addClassCleanup(cls.check_inputs)
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-modbus-status-fields-")
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(encoding="utf-8")
        oracle = (CPP / "modbus_status_fields_oracle.inc").read_text(encoding="utf-8")
        actual, original = [], []
        for signature, expected in SIGNATURES:
            current = definition(source, signature)
            frozen = definition(oracle, signature)
            reversed_body, count = re.subn(
                r'FastBee::setFixedJsonField\((data|item|doc), "([^"]+)", ([^\n]+?)\);',
                lambda match: match[1] + '["' + match[2] + '"] = ' + match[3] + ';', current)
            if count != expected or reversed_body != frozen:
                raise AssertionError("Modbus body differs beyond reviewed fixed scalar writes: " + signature)
            # Identical explicit allocator/capture seam in both complete bodies.
            # No production call/return/control flow or root-document write changes.
            allocation = "JsonDocument doc;"
            injected = "JsonDocument doc(activeAllocator); DocumentCapture capture(doc);"
            if current.count(allocation) != 1 or frozen.count(allocation) != 1:
                raise AssertionError("Whole method document seam changed")
            actual.append(current.replace(allocation, injected))
            original.append(frozen.replace(allocation, injected).replace(
                "ModbusHandler::", "OracleModbusHandler::"))
        (folder / "modbus_status_actual.inc").write_text("\n\n".join(actual), encoding="utf-8")
        (folder / "modbus_status_original.inc").write_text("\n\n".join(original), encoding="utf-8")
        header = (ROOT / "include/protocols/ModbusHandler.h").read_text(encoding="utf-8")
        constants = (ROOT / "include/core/SystemConstants.h").read_text(encoding="utf-8")
        max_tasks = re.findall(
            r"^\s*constexpr uint8_t\s+MODBUS_MAX_POLL_TASKS\s*=\s*\d+\s*;", constants, re.M)
        if len(max_tasks) != 1:
            raise AssertionError("Production max-tasks constant seam changed")
        types = "namespace Protocols {\n" + max_tasks[0] + "\n}\n"
        types += definition(header, "enum ModbusMode") + ";\n"
        master = definition(header, "struct MasterConfig")
        config = definition(header, "struct ModbusConfig")
        types += "struct FixtureMaster {\n" + declaration(master, "taskCount")
        types += declaration(master, "deviceCount") + "};\nstruct FixtureConfig {\n"
        for field in ("mode", "slaveAddress", "baudRate", "txPin", "rxPin", "dePin"):
            types += declaration(config, field)
        types += "FixtureMaster master;\n};\nstruct ModbusFields {\n"
        types += declaration(header, "isInitialized") + "FixtureConfig config;\n"
        types += "using StatusChangeCallback = std::function<void(const String&)>;\n"
        types += declaration(header, "_statusChangeCallback")
        for field in ("_lastStatusHash", "_totalPollCount", "_successPollCount", "_failedPollCount",
                      "_timeoutPollCount", "_lastPollTime", "_slaveBackoffSkips", "_slaveBackoffEvictions"):
            types += declaration(header, field)
        types += definition(header, "struct SlaveBackoffState") + ";\n"
        slots = re.findall(r"static constexpr uint8_t SLAVE_BACKOFF_SLOT_COUNT\s*=\s*"
                           r"Protocols::MODBUS_MAX_POLL_TASKS\s*\+\s*4\s*;", header)
        if len(slots) != 1:
            raise AssertionError("Backoff slot constant seam changed")
        types += slots[0] + "\nSlaveBackoffState _slaveBackoff[SLAVE_BACKOFF_SLOT_COUNT];\n"
        types += definition(header, "uint32_t getLastPollAgeSec(").replace(
            "getLastPollAgeSec(", "getLastPollAgeSecBody(") + "\n"
        types += "String getStatusBody() const;\nString getStatus() const;\n"
        types += "uint32_t getLastPollAgeSec() const;\nstatic bool _deadlineReached(uint32_t,uint32_t);\n};\n"
        (folder / "modbus_status_types.inc").write_text(types, encoding="utf-8")
        dependencies = definition(source, "String ModbusHandler::getStatus(").replace(
            "ModbusHandler::getStatus(", "ModbusFields::getStatusBody(")
        dependencies += "\n\n" + definition(source, "bool ModbusHandler::_deadlineReached(").replace(
            "ModbusHandler::", "ModbusFields::")
        (folder / "modbus_status_dependencies.inc").write_text(dependencies, encoding="utf-8")
        (folder / "fixture_prefix.h").write_text(
            "#include <Arduino.h>\n#undef malloc\n#undef free\n", encoding="utf-8")
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.env.get("PATH", "")
        cls.binary = folder / "modbus-status-fields.exe"
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-I" + str(folder), "-I" + str(ROOT / "test/unit/cpp"),
            "-I" + str(SUPPORT / "test/mocks"), "-I" + str(ROOT / "include"), "-I" + str(library),
            "-include", str(folder / "fixture_prefix.h"), str(CPP / "modbus_status_fields_harness.cpp"),
            str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=120, env=cls.env)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def check_inputs(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError("Modbus status production/oracle/library/fixture input drift")

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120, env=self.env)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PASS " + mode, run.stdout)

    def test_notify(self):
        self.check("notify")

    def test_statistics(self):
        self.check("statistics")


if __name__ == "__main__":
    unittest.main()
