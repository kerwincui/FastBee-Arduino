"""Full real HealthMonitor writers versus frozen originals with actual AJ faults.

SDK/getter values are fixtures. No hardware access, long-duration acceptance or
firmware-size claim is made by these tests.
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
SIGNATURES = ("void HealthMonitor::fillDegradationJson(", "void HealthMonitor::fillMetricsJson(")
ORACLE_SHA256 = "e81bcc9644e4e7b35ac39ad49c62d58bd9c3c0740bace05cb7e89233df4090e5"
MEMBERS = ("currentHealth", "_currentLevel", "mqttQueueDepth", "sseClientCount", "_dramHistoryCount",
           "pollDurationMs", "_mqttsMemoryDowngrade", "_mqttStoppedForMemory", "_mqttDisabledForMemory",
           "_mqttRestorePendingForMemory", "_modbusStoppedForMemory", "_periphExecPausedForMemory",
           "_optionalPeripheralsSuspendedForMemory", "_mqttRecoveryBlockedForStorage",
           "_memorySuspendedPeripheralCount")


def rewrite_fixed_assignments(source):
    pattern = re.compile(r'\b(doc|dst)((?:\["[^"\n]+"\]){1,2})[ \t]*=\s*([^;]+);')
    def replacement(match):
        keys = re.findall(r'\["([^"\n]+)"\]', match[2])
        return "FastBee::setFixedJsonField(" + match[1] + ", " + ", ".join(
            '"' + key + '"' for key in keys) + ", " + match[3] + ");"
    return pattern.subn(replacement, source)


def state_declarations(header):
    declarations = []
    for name in MEMBERS:
        matches = re.findall(r'^\s*((?:SystemHealth|MemoryGuardLevel|bool|uint\d+_t)\s+' +
            re.escape(name) + r'(?:\s*=\s*[^;\n]+)?\s*;)', header, re.M)
        if len(matches) != 1:
            raise AssertionError("Expected one exact Health state declaration: " + name)
        declaration = matches[0]
        if "=" not in declaration:
            declaration = declaration[:-1] + "{};"
        declarations.append(declaration)
    if len(re.findall(r'\bMAX_TRACKED_POOLS\s*=\s*4\s*;', header)) != 1:
        raise AssertionError("Pool fixture capacity changed")
    return "\n".join(declarations)


def injectable_methods(functions, suffix):
    result = []
    for method in functions:
        method = method.replace("HealthMonitor::", "Health" + suffix + "::", 1)
        if "fillMetricsJson(" in method:
            if method.count("JsonDocument doc;") != 1:
                raise AssertionError("Expected one metrics document allocator seam")
            method = method.replace("JsonDocument doc;", "JsonDocument doc(healthAllocator);", 1)
        result.append(method)
    return "\n\n".join(result)


class HealthFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered ArduinoJson headers required")
        source_path = ROOT / "src/systems/HealthMonitor.cpp"
        header_path = ROOT / "include/systems/HealthMonitor.h"
        oracle_path = HERE / "cpp/health_fixed_json_before.inc"
        harness_path = HERE / "cpp/health_fixed_json_harness.cpp.in"
        helper = ROOT / "src/network/FixedJsonField.cpp"
        helper_header = ROOT / "include/network/FixedJsonField.h"
        prelude = HERE / "cpp/fixed_json_document_prelude.h"
        probe = HERE / "cpp/document_json_probe.h"
        mock = SUPPORT / "test/mocks/Arduino.h"
        support_code = HERE / "test_periph_exec_fixed_json.py"
        source = source_path.read_text(encoding="utf-8")
        header = header_path.read_text(encoding="utf-8")
        oracle = oracle_path.read_text(encoding="utf-8")
        if hashlib.sha256(oracle.encode("utf-8")).hexdigest() != ORACLE_SHA256:
            raise AssertionError("Frozen Health two-method oracle changed")
        old = [definition(oracle, signature) for signature in SIGNATURES]
        current = [definition(source, signature) for signature in SIGNATURES]
        rewritten = [rewrite_fixed_assignments(method) for method in old]
        if [count for _, count in rewritten] != [17, 37]:
            raise AssertionError("Reviewed 54 fixed Health assignments changed")
        if current != [method for method, _ in rewritten]:
            raise AssertionError("Health bodies drifted beyond the reviewed fixed assignments")
        replacements = {
            "@HEALTH_TYPES@": definition(header, "enum class MemoryGuardLevel") + ";\n" +
                               definition(header, "struct SystemHealth") + ";",
            "@POOL_TYPE@": definition(header, "struct PoolStatsEntry") + ";",
            "@HEALTH_MEMBERS@": state_declarations(header),
            "@HEALTH_METHODS@": injectable_methods(old, "Oracle") + "\n\n" +
                                injectable_methods(current, "Current"),
        }
        program = harness_path.read_text(encoding="utf-8")
        for marker, value in replacements.items():
            if program.count(marker) != 1:
                raise AssertionError("Expected one Health fixture marker: " + marker)
            program = program.replace(marker, value, 1)
        roots = (HERE / "cpp", ROOT / "include", SUPPORT / "include", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), source_path, header_path, oracle_path, harness_path,
                 helper, helper_header, prelude, probe, mock, support_code)
        cls.inputs = sorted(local_includes(seeds, roots) |
            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-health-json-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        arduino = mock.read_text(encoding="utf-8")
        marker = "void clear() { _value.clear(); }"
        if arduino.count(marker) != 1:
            raise AssertionError("Arduino String writer fixture changed")
        arduino = arduino.replace(marker, marker +
            '\n    bool concat(const char* p) { _value += p ? p : ""; return true; }', 1)
        (temporary / "Arduino.h").write_text(arduino, encoding="utf-8")
        cpp = temporary / "health-json.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        cls.binaries = {}
        for feature in (0, 1):
            binary = temporary / ("health-" + str(feature) + (".exe" if os.name == "nt" else ""))
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
                "-DFASTBEE_ENABLE_PERIPH_EXEC=" + str(feature),
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                "-I" + str(temporary), *["-I" + str(path) for path in roots],
                "-include", str(prelude), str(cpp), str(helper), "-o", str(binary)]
            built = subprocess.run(command, text=True, capture_output=True, encoding="utf-8",
                                   errors="replace", timeout=120, env=cls.environment)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries[feature] = binary

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("Health JSON verification inputs changed during the run")

    def check(self, mode, feature):
        result = subprocess.run([str(self.binaries[feature]), mode], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _feature in (0, 1):
    for _mode in ("metrics", "degradation", "metrics-oom", "degradation-oom"):
        setattr(HealthFixedJsonTests, "test_" + str(_feature) + "_" + _mode.replace("-", "_"),
                lambda self, mode=_mode, feature=_feature: self.check(mode, feature))

if __name__ == "__main__":
    unittest.main()
