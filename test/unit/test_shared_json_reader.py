"""Real custom-reader entry equivalence through the shared parser.

Run only after applying the separately reviewed runtime draft. Reuses the
existing real-ArduinoJson consumed-reader corpus/allocator, but freezes the
previous RF/Modbus/config entry bodies independently in shared_reader_legacy.inc.
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

ROOT = Path(os.environ.get("FASTBEE_TEST_ROOT", Path(__file__).resolve().parents[2]))
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))


def replace_once(text, before, after):
    if text.count(before) != 1:
        raise AssertionError("Ambiguous reviewed source seam: " + before)
    return text.replace(before, after, 1)


class SharedJsonReaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE",
                       SUPPORT / ".pio/libdeps/native/ArduinoJson/src"))
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson required")
        paths = (
            "src/network/handlers/ModbusHttpOutput.h",
            "src/peripherals/drivers/RFLearnDriver.cpp",
            "src/core/PeriphExecConfigStore.cpp",
            "src/protocols/MQTTClient.cpp",
            "include/peripherals/drivers/RFLearnDriver.h",
            "include/core/SystemConstants.h",
            "include/utils/ConsumedStringJsonReader.h",
            "include/utils/SharedJsonDeserializer.h",
            "src/utils/SharedJsonDeserializer.cpp",
            "test/unit/cpp/json_consumed_reader_dedup_harness.cpp",
            "test/unit/cpp/shared_reader_legacy.inc",
            "test/unit/cpp/shared_reader_config_checks.inc",
            "test/unit/test_periph_exec_config_persistence.py",
        )
        cls.inputs = [Path(__file__)] + [ROOT / path for path in paths]
        cls.inputs += [SUPPORT / "test/mocks/Arduino.h"]
        cls.inputs += sorted(library.rglob("*.hpp")) + sorted(library.rglob("*.h"))
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.addClassCleanup(cls.check_inputs)
        files = {path: (ROOT / path).read_text(encoding="utf-8") for path in paths}
        modbus, rf, config, mqtt, rf_types, constants = [files[path] for path in paths[:6]]
        legacy = files["test/unit/cpp/shared_reader_legacy.inc"]
        register = cpp_function(modbus, "inline bool registerArray(")
        rf_helpers = "\n".join(cpp_function(rf, sig) for sig in
                              ("bool validRfName(", "bool parseRfKeyCode(", "bool parseRfKeys("))
        config_reader = cpp_function(config, "class ConfigValueReader") + ";"
        config_error = cpp_function(config, "static bool configError(")
        config_entry = cpp_function(config, "static bool readConfigValue(")
        for current, signature in ((register, "inline bool registerArray("),
                (cpp_function(rf, "bool parseRfKeys("), "bool parseRfKeys("),
                (config_entry, "static bool readConfigValue(")):
            previous = cpp_function(legacy, signature)
            if replace_once(current, "FastBee::deserializeJsonShared(", "deserializeJson(") != previous:
                raise AssertionError("Actual entry changed beyond the reviewed call: " + signature)
        if config_reader != cpp_function(legacy, "class ConfigValueReader") + ";":
            raise AssertionError("ConfigValueReader changed; review legacy oracle")
        aliases = []
        for source, name in ((modbus, "JsonReader"), (rf, "RfJsonReader")):
            values = re.findall(r"using " + name + r"\s*=\s*([^;]+);", source)
            if values != ["FastBee::ConsumedStringJsonReader"]:
                raise AssertionError("Reviewed reader alias changed")
            aliases.append("using " + name + " = FastBee::ConsumedStringJsonReader;")
        mqtt_helpers = mqtt[mqtt.index("static bool mqttPayloadIsStructuredJson("):
                            mqtt.index("static uint8_t currentModbusTransferType(")]
        start = mqtt.index("        String enqueueMsg = message;")
        stop = mqtt.index("        if (shouldEnqueue && _dataCommandQueue)", start)
        route = ("static RouteResult route(const String& message, uint8_t modbusTransferType, "
                 "bool isRawModbusCommand) {\n" + mqtt[start:stop] +
                 "return {shouldEnqueue, std::string(enqueueMsg.c_str(), enqueueMsg.length())};\n}")
        capacity = re.findall(r"constexpr uint16_t MODBUS_BUFFER_SIZE = [0-9]+;", constants)
        if len(capacity) != 1:
            raise AssertionError("Ambiguous production RTU capacity")
        generated = cpp_function(rf_types, "struct RfKeyEntry") + ";\n"
        generated += "namespace Protocols { " + capacity[0] + " }\n"
        generated += files["test/unit/cpp/shared_reader_config_checks.inc"].split("// @CONFIG_CHECKS@")[0]
        generated += '#include "shared_reader_legacy.inc"\n'
        generated += ("namespace Current {\n" + "\n".join(aliases) + "\n" + register + "\n" +
                      rf_helpers + "\n" + mqtt_helpers + "\n" + route + "\n}\n")
        generated += "namespace Original {\n" + mqtt_helpers + "\n" + route + "\n}\n"
        generated += "namespace ConfigCurrent {\n" + config_reader + "\n" + config_error + "\n" + config_entry + "\n}\n"
        for name, source, prefix in (
                ("CurrentFault", rf_helpers, "\n".join(aliases)),
                ("OriginalFault", "\n".join(cpp_function(legacy, sig) for sig in
                 ("bool validRfName(", "bool parseRfKeyCode(", "bool parseRfKeys(")),
                 "using RfJsonReader = Original::RfJsonReader;")):
            probe = replace_once(cpp_function(mqtt_helpers, "static bool mqttPayloadIsStructuredJson("),
                                 "JsonDocument probe;", "JsonDocument probe(allocator);")
            fault_rf = replace_once(source, "JsonDocument doc; RfJsonReader reader{raw};",
                                    "JsonDocument doc(allocator); RfJsonReader reader{raw};")
            generated += ("namespace " + name + " {\n" + prefix +
                          "\nstatic ArduinoJson::Allocator* allocator = nullptr;\n" +
                          fault_rf + "\n" + probe + "\n}\n")
        harness = files["test/unit/cpp/json_consumed_reader_dedup_harness.cpp"]
        harness = replace_once(harness, '#include "utils/ConsumedStringJsonReader.h"',
                               '#include "utils/ConsumedStringJsonReader.h"\n#include "utils/SharedJsonDeserializer.h"\n#include <cctype>')
        # The production API uses only the default limit. Existing explicit-limit
        # regressions remain legacy coverage; shared-parser coverage uses defaults.
        harness = replace_once(harness,
            "Current::JsonReader reader{text}; error = deserializeJson(doc, reader, limit);",
            "Current::JsonReader reader{text};\n"
            "            error = nesting == ARDUINOJSON_DEFAULT_NESTING_LIMIT ?\n"
            "                FastBee::deserializeJsonShared(doc, reader) : deserializeJson(doc, reader, limit);")
        checks = files["test/unit/cpp/shared_reader_config_checks.inc"].split("// @CONFIG_CHECKS@")
        if len(checks) != 2:
            raise AssertionError("Ambiguous config check marker")
        harness = replace_once(harness, "int main(int argc, char** argv) {", checks[1] + "\nint main(int argc, char** argv) {")
        harness = replace_once(harness, 'if (mode == "readers") readers();',
                               'if (mode == "config") configCases();\n    else if (mode == "config-oom") configOom();\n    else if (mode == "readers") readers();')
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-shared-reader-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        (directory / "production_entries.inc").write_text(generated, encoding="utf-8")
        (directory / "shared_reader_legacy.inc").write_text(legacy, encoding="utf-8")
        source = directory / "shared-reader.cpp"
        source.write_text(harness, encoding="utf-8")
        cls.binary = directory / "shared-reader.exe"
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.env.get("PATH", "")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-I" + str(directory), "-I" + str(SUPPORT / "test/mocks"),
            "-I" + str(ROOT / "include"), "-I" + str(library), str(source),
            str(ROOT / "src/utils/SharedJsonDeserializer.cpp"), "-o", str(cls.binary)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120, env=cls.env)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def check_inputs(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError("Shared-reader differential inputs changed")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=120, env=self.env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ("readers", "parser", "oom", "entries", "entry-oom", "config", "config-oom"):
    setattr(SharedJsonReaderTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
