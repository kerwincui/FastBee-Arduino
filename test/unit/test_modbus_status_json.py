"""Actual Modbus status/diagnostic JSON versus the frozen original methods."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import modbus_document_field_support as document_fields

ROOT = Path(__file__).resolve().parents[2]
SOURCE = Path(os.environ.get("FASTBEE_TEST_MODBUS_SOURCE", ROOT))
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))


def method(source, marker):
    if source.count(marker) != 1:
        raise AssertionError("Expected one function: " + marker)
    start = source.index(marker)
    return source[start:source.index("\n}", start) + 2]


class ModbusStatusJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        library = SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not Path(compiler).is_file() or not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("C++17 compiler and registered native ArduinoJson required")
        cls.inputs = [SOURCE / name for name in (
            "src/network/handlers/ModbusRouteHandler.cpp",
            "include/network/FixedJsonField.h", "src/network/FixedJsonField.cpp",
            "include/network/handlers/HandlerUtils.h",
            "include/protocols/ModbusReadDiagnostics.h", "include/core/SystemConstants.h")]
        cls.inputs += [ROOT / name for name in (
            "test/unit/test_modbus_status_json.py",
            "test/unit/cpp/modbus_status_json_harness.cpp.in",
            "test/unit/cpp/modbus_status_json_legacy.inc")]
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.inputs += document_fields.inputs()
        cls.hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}
        source = cls.inputs[0].read_text(encoding="utf-8")
        signatures = (
            "static uint8_t countEnabledPollTasks(",
            "static uint16_t getMinEnabledPollInterval(",
            "static uint16_t getMaxEnabledPollInterval(",
            "static uint32_t getTaskCacheAgeSec(",
            "static uint32_t getTaskStaleThresholdSec(",
            "static float calcRate(",
            "static const char* getTaskHealthStatus(",
            "static const char* buildModbusRiskLevel(",
            "static void addModbusReadDiagnostics(",
            "static void addModbusDebug(",
            "void ModbusRouteHandler::handleGetModbusStatus(")
        current = "\n\n".join(method(source, marker) for marker in signatures)
        document_fields.oracle_program(current, ("handleGetModbusStatus",))
        sender = method(cls.inputs[3].read_text(encoding="utf-8"), "inline void sendJsonStream(")
        sender = sender.replace("inline void sendJsonStream(", "inline void sendJsonStreamProduction(", 1)
        constants = cls.inputs[5].read_text(encoding="utf-8")
        declarations = []
        for name in ("MODBUS_MAX_POLL_TASKS", "MODBUS_ONESHOT_BUFFER_SIZE",
                     "MODBUS_POLL_LABEL_MAX_LEN", "MODBUS_MAX_MAPPINGS_PER_TASK",
                     "MODBUS_SENSOR_ID_MAX_LEN"):
            matches = re.findall(r"constexpr\s+uint8_t\s+" + name + r"\s*=\s*\d+\s*;", constants)
            if len(matches) != 1:
                raise AssertionError("Missing/changed production constant: " + name)
            declarations.append(matches[0])
        fixture = cls.inputs[7].read_text(encoding="utf-8")
        replacements = {
            "@CONSTANTS@": "namespace Protocols {\n" + "\n".join(declarations) + "\n}",
            "@SEND@": sender,
            "@CURRENT@": current,
            "@LEGACY@": cls.inputs[8].read_text(encoding="utf-8")}
        for marker, body in replacements.items():
            if fixture.count(marker) != 1:
                raise AssertionError("Missing/duplicate fixture marker: " + marker)
            fixture = fixture.replace(marker, body)
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-modbus-status-json-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temp = Path(cls.temporary.name)
        (temp / "harness.cpp").write_text(fixture, encoding="utf-8")
        cls.binary = temp / "status.exe"
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
            "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-I" + str(SOURCE / "include"), "-I" + str(library),
            str(temp / "harness.cpp"), str(SOURCE / "src/network/FixedJsonField.cpp"),
            "-o", str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", env=cls.environment, timeout=120)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}:
            raise AssertionError("Modbus JSON comparison inputs changed")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding="utf-8", errors="replace", env=self.environment, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_actual_full_compact_master_slave_and_absent_status_bytes(self):
        self.check("status")

    def test_actual_status_allocation_failures_match_original_and_send_503(self):
        self.check("status-oom")

    def test_actual_request_diagnostics_and_debug_copy_mutable_prefixes(self):
        self.check("diagnostics")

    def test_actual_diagnostics_allocation_failures_match_original(self):
        self.check("diagnostics-oom")


if __name__ == "__main__":
    unittest.main()
