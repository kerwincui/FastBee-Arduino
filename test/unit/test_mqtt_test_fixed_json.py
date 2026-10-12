"""Differential full MQTT probe staging plus real recovery/CAS on a host filesystem.

The production report-instance allocator remains covered by test_mqtt_test_recovery;
this twin-run fixture supplies a deterministic sequence for identical JSON IDs.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_periph_exec_fixed_json import definition, local_includes

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
SIGNATURES = ("static bool backupMqttConfig(", "static bool saveMqttTestConfig(")
ORACLE_SHA256 = "c0ae4e93194f120b10c7762fd8f85c77034aaa3fc81ba3db34d368a371fb5f41"


def fixed_assignments(source):
    return re.subn(r'\b(backupDoc|mqtt)\["([^"\n]+)"\][ \t]*=\s*([^;]+);',
        lambda match: 'FastBee::setFixedJsonField(' + match[1] + ', "' + match[2] + '", ' +
                      match[3] + ');', source)


def explicit_allocator(source):
    # Same allocator seam in both original and current full methods; no decision,
    # comparison, serialization, or filesystem behavior is replaced.
    return re.sub(r'\bJsonDocument ([A-Za-z_]\w*);', r'JsonDocument \1(mqttJsonAllocator);', source)


def writer_methods(methods, suffix):
    result = []
    for method in methods:
        method = method.replace("backupMqttConfig(", "backupMqttConfig" + suffix + "(")
        method = method.replace("saveMqttTestConfig(", "saveMqttTestConfig" + suffix + "(")
        method = method.replace("    const size_t expected = measureJson(backupDoc);",
            '    recordDocument("backup", backupDoc);\n    const size_t expected = measureJson(backupDoc);')
        method = method.replace("    if (candidate.overflowed() ||",
            '    recordDocument("candidate", candidate);\n    if (candidate.overflowed() ||')
        result.append(explicit_allocator(method))
    return "\n\n".join(result)


class MqttTestFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered ArduinoJson headers required")
        source_path = ROOT / "src/network/handlers/MqttRouteHandler.cpp"
        storage_path = ROOT / "src/systems/ConfigStorage.cpp"
        oracle_path = HERE / "cpp/mqtt_test_fixed_json_before.inc"
        harness_path = HERE / "cpp/mqtt_test_fixed_json_harness.cpp.in"
        recovery_path = HERE / "cpp/mqtt_test_recovery_harness.cpp.in"
        probe_path = HERE / "cpp/document_json_probe.h"
        helper = ROOT / "src/network/FixedJsonField.cpp"
        helper_header = ROOT / "include/network/FixedJsonField.h"
        support_code = HERE / "test_periph_exec_fixed_json.py"
        source = source_path.read_text(encoding="utf-8")
        storage = storage_path.read_text(encoding="utf-8")
        oracle = oracle_path.read_text(encoding="utf-8")
        if hashlib.sha256(oracle.encode("utf-8")).hexdigest() != ORACLE_SHA256:
            raise AssertionError("Frozen MQTT staging methods changed")
        old = [definition(oracle, signature) for signature in SIGNATURES]
        current = [definition(source, signature) for signature in SIGNATURES]
        rewritten = [fixed_assignments(method) for method in old]
        if [count for _, count in rewritten] != [3, 13]:
            raise AssertionError("Reviewed MQTT 16 literal assignments changed")
        if current != [method for method, _ in rewritten]:
            raise AssertionError("MQTT staging drifted beyond the 16 reviewed assignments")
        shared = "\n\n".join(definition(storage, signature) for signature in (
            "bool ConfigStorage::saveJSONConfig(", "bool ConfigStorage::loadJSONConfig(",
            "bool ConfigStorage::compareExchangeMqttConfig("))
        shared += "\n\n" + "\n\n".join(definition(source, signature) for signature in (
            "static bool restoreMqttConfigFromBackup(", "void MqttRouteHandler::checkPendingTestRestore("))
        methods = explicit_allocator(shared) + "\n\n" + writer_methods(old, "Oracle") + "\n\n" + writer_methods(current, "Current")
        recovery = recovery_path.read_text(encoding="utf-8")
        if recovery.count("@METHODS@") != 1:
            raise AssertionError("Recovery fixture boundary changed")
        recovery = recovery.split("@METHODS@", 1)[0]
        marker = "        if(failSave)return false;"
        if recovery.count(marker) != 1:
            raise AssertionError("Recovery save seam changed")
        recovery = recovery.replace(marker, '        recordDocument("save", doc);\n' + marker, 1)
        marker = "        return !deserializeJson(doc,disk[path]);"
        if recovery.count(marker) != 1:
            raise AssertionError("Recovery load seam changed")
        recovery = recovery.replace(marker, '        const bool loaded=!deserializeJson(doc,disk[path]);\n'
            '        recordDocument("load",doc);return loaded;', 1)
        replacements = {
            "@ALLOCATOR@": definition(probe_path.read_text(encoding="utf-8"), "class DocumentProbeAllocator") + ";",
            "@RECOVERY_FIXTURE@": recovery,
            "@METHODS@": methods,
        }
        program = harness_path.read_text(encoding="utf-8")
        for marker, value in replacements.items():
            if program.count(marker) != 1:
                raise AssertionError("Expected one MQTT fixture marker: " + marker)
            program = program.replace(marker, value, 1)
        roots = (HERE / "cpp", ROOT / "include", SUPPORT / "include", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), source_path, storage_path, oracle_path, harness_path,
                 recovery_path, probe_path, helper, helper_header, support_code)
        cls.inputs = sorted(local_includes(seeds, roots) |
            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-mqtt-test-json-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        cpp = temporary / "mqtt-test-json.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.binary = temporary / ("mqtt-test-json.exe" if os.name == "nt" else "mqtt-test-json")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            *["-I" + str(path) for path in roots], str(cpp), str(helper), "-o", str(cls.binary)]
        built = subprocess.run(command, text=True, capture_output=True, encoding="utf-8",
                               errors="replace", timeout=120, env=cls.environment)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("MQTT fixed JSON verification inputs changed during the run")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)

    def test_values_and_recovery(self):
        self.check("values")

    def test_allocation_faults_and_recovery(self):
        self.check("allocation-faults")


if __name__ == "__main__":
    unittest.main()
