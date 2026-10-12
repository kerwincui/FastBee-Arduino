"""Actual bounded-byte and consumed-String reader equivalence.

Uses real ArduinoJson 7.4.2, actual unchanged
business entry bodies, and preserved pre-dedup reader definitions in the C++
fixture. No device, serial, or firmware access.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))


def one_replace(text, before, after):
    if text.count(before) != 1:
        raise AssertionError("Production extraction seam changed: " + before)
    return text.replace(before, after)


class JsonConsumedReaderDedupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise unittest.SkipTest("C++17 compiler required")
        library = SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise AssertionError("Registered native ArduinoJson dependency required")
        cls.inputs = [Path(__file__).resolve()] + [ROOT / name for name in (
            "test/unit/cpp/json_consumed_reader_dedup_harness.cpp",
            "include/utils/ConsumedStringJsonReader.h",
            "src/network/handlers/ModbusHttpOutput.h",
            "src/peripherals/drivers/RFLearnDriver.cpp",
            "src/protocols/MQTTClient.cpp",
            "include/peripherals/drivers/RFLearnDriver.h",
            "include/core/SystemConstants.h",
            "test/unit/test_periph_exec_config_persistence.py",
        )] + [SUPPORT / "test/mocks/Arduino.h"]
        cls.inputs += [ROOT / "include/utils/SharedJsonDeserializer.h", ROOT / "src/utils/SharedJsonDeserializer.cpp"]
        cls.inputs += sorted(library.rglob("*.hpp")) + sorted(library.rglob("*.h"))
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.addClassCleanup(cls.check_inputs)
        modbus = cls.inputs[3].read_text(encoding="utf-8")
        rf = cls.inputs[4].read_text(encoding="utf-8")
        mqtt = cls.inputs[5].read_text(encoding="utf-8")
        rf_types = cls.inputs[6].read_text(encoding="utf-8")
        constants = cls.inputs[7].read_text(encoding="utf-8")
        aliases = []
        for source, name in ((modbus, "JsonReader"), (rf, "RfJsonReader")):
            found = re.findall(r"using " + name + r"\s*=\s*([^;]+);", source)
            if len(found) != 1 or found[0] != "FastBee::ConsumedStringJsonReader":
                raise AssertionError("Actual callers must use the same concrete consumed reader")
            aliases.append("using " + name + " = " + found[0] + ";")
        register = cpp_function(modbus, "inline bool registerArray(")
        rf_helpers = "\n".join(cpp_function(rf, signature) for signature in (
            "bool validRfName(", "bool parseRfKeyCode(", "bool parseRfKeys("))
        mqtt_helpers = mqtt[mqtt.index("static bool mqttPayloadIsStructuredJson("):
                            mqtt.index("static uint8_t currentModbusTransferType(")]
        candidate_parse = "deserializeJson(probe, reinterpret_cast<const char*>(payload), length)"
        legacy_mqtt = one_replace(mqtt_helpers, candidate_parse,
                                  "deserializeJson(probe, payload, length)")
        # Actual DATA_COMMAND classification/wrapping block, before queue I/O.
        start = mqtt.index("        String enqueueMsg = message;")
        stop = mqtt.index("        if (shouldEnqueue && _dataCommandQueue)", start)
        route = ("static RouteResult route(const String& message, uint8_t modbusTransferType, "
                 "bool isRawModbusCommand) {\n" + mqtt[start:stop] +
                 "return {shouldEnqueue, std::string(enqueueMsg.c_str(), enqueueMsg.length())};\n}")
        generated = cpp_function(rf_types, "struct RfKeyEntry") + ";\n"
        capacity = re.findall(r"constexpr uint16_t MODBUS_BUFFER_SIZE = [0-9]+;", constants)
        if len(capacity) != 1:
            raise AssertionError("Ambiguous production RTU buffer size")
        generated += "namespace Protocols { " + capacity[0] + " }\n"
        for name, prefix, helpers in (("Current", "\n".join(aliases), mqtt_helpers),
                                      ("Original", "", legacy_mqtt)):
            selected_register = register if name == "Current" else one_replace(register,
                "FastBee::deserializeJsonShared(values, reader)", "deserializeJson(values, reader)")
            selected_rf = rf_helpers if name == "Current" else one_replace(rf_helpers,
                "FastBee::deserializeJsonShared(doc, reader)", "deserializeJson(doc, reader)")
            generated += ("namespace " + name + " {\n" + prefix + "\n" + selected_register + "\n" +
                          selected_rf + "\n" + helpers + "\n" + route + "\n}\n")
        # The exact original entry bodies also run with an explicit allocator.
        # Only JsonDocument constructor injection differs; default-allocator
        # Current/Original above are separately executed unchanged.
        for name, prefix, helpers in (("CurrentFault", "\n".join(aliases), mqtt_helpers),
                                      ("OriginalFault", "using JsonReader = Original::JsonReader;\n"
                                       "using RfJsonReader = Original::RfJsonReader;", legacy_mqtt)):
            probe = cpp_function(helpers, "static bool mqttPayloadIsStructuredJson(")
            probe = one_replace(probe, "JsonDocument probe;", "JsonDocument probe(allocator);")
            selected_rf = rf_helpers if name == "CurrentFault" else one_replace(rf_helpers,
                "FastBee::deserializeJsonShared(doc, reader)", "deserializeJson(doc, reader)")
            fault_rf = one_replace(selected_rf, "JsonDocument doc; RfJsonReader reader{raw};",
                                  "JsonDocument doc(allocator); RfJsonReader reader{raw};")
            generated += ("namespace " + name + " {\n" + prefix +
                          "\nstatic ArduinoJson::Allocator* allocator = nullptr;\n" +
                          fault_rf + "\n" + probe + "\n}\n")
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-consumed-reader-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        (directory / "production_entries.inc").write_text(generated, encoding="utf-8")
        cls.binary = directory / "consumed-reader.exe"
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.env.get("PATH", "")
        build = subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-I" + str(directory), "-I" + str(SUPPORT / "test/mocks"),
            "-I" + str(ROOT / "include"), "-I" + str(library),
            str(cls.inputs[1]), str(ROOT / "src/utils/SharedJsonDeserializer.cpp"), "-o", str(cls.binary)], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=90, env=cls.env)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def check_inputs(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError("Consumed JSON reader production/library/fixture input drift")

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=90, env=self.env)
        self.assertEqual(0, run.returncode, run.stdout + run.stderr)

    def test_reader_read_span_unsigned_offset_eof_and_whitespace(self):
        self.check("readers")

    def test_actual_arduinojson_errors_partial_dom_nul_bytes_lengths_and_nesting(self):
        self.check("parser")

    def test_ownership_and_allocation_growth_oom_traces_match(self):
        self.check("oom")

    def test_actual_mqtt_branch_and_crc_raw_precedence(self):
        self.check("mqtt")

    def test_actual_modbus_array_and_rf_keys_reject_second_root_and_trailing_bytes(self):
        self.check("entries")

    def test_actual_entries_allocator_failure_and_partial_state(self):
        self.check("entry-oom")


if __name__ == "__main__":
    unittest.main()
