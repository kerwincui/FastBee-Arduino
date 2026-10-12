"""Real non-pending save/load bodies, byte storage and a frozen serializer oracle.

Hardware admission, locks and the atomic-storage primitive are explicit seams.
The complete production JSON serializer/parser/save/load functions execute.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))


def definition(source, signature):
    if source.count(signature) != 1:
        raise AssertionError("Expected exactly one production definition: " + signature)
    begin = source.index(signature)
    opening = source.index("{", begin)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[begin:token.end()]
    raise AssertionError("Unterminated production function: " + signature)


def scalar_draft(baseline):
    pattern = re.compile(r'^(\s*)(obj|params|namedPins)\["([^"\n]+)"\]\s*=\s*([^\n]+);$', re.M)
    calls = []

    def replace(match):
        indent, target, key, rhs = match.groups()
        calls.append((target, key, rhs))
        return f'{indent}FastBee::setFixedJsonField({target}, "{key}", {rhs});'

    result = pattern.sub(replace, baseline)
    if len(calls) != 144 or {name: sum(call[0] == name for call in calls)
                             for name in ("obj", "params", "namedPins")} != {
                                 "obj": 4, "params": 124, "namedPins": 16}:
        raise AssertionError("Frozen scalar draft must have the reviewed 144 call sites")
    return result


class PeripheralPersistenceRoundtripTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        configured = os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE")
        library = Path(configured) if configured else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson headers required")
        baseline_path = HERE / "cpp/peripheral_persistence_serializer_oracle.inc"
        harness_path = HERE / "cpp/peripheral_persistence_roundtrip_harness.cpp.in"
        source_path = ROOT / "src/core/PeripheralManagerPersistence.cpp"
        source = source_path.read_text(encoding="utf-8")
        baseline = baseline_path.read_text(encoding="utf-8").strip()
        drafted = scalar_draft(baseline)
        # Preserve the original 144-site oracle file. The only reviewed schema
        # extension below is independently spelled out for RTC boot policy.
        marker = '        params["trickleCharge"] = config.params.ds1302.trickleCharge;'
        extra = '\n        params["bootSeed"] = config.params.ds1302.bootSeed;\n' + \
            '        if (config.params.ds1302.utcOffsetMinutes != INT16_MIN)\n' + \
            '            params["utcOffsetMinutes"] = config.params.ds1302.utcOffsetMinutes;'
        if baseline.count(marker) != 1:
            raise AssertionError("RTC oracle marker changed")
        baseline = baseline.replace(marker, marker + extra)
        fixed_marker = '        FastBee::setFixedJsonField(params, "trickleCharge", config.params.ds1302.trickleCharge);'
        delegated = '\n        FastBee::serializeRtcBootSeedConfig(params, config.params.ds1302.bootSeed,\n' + \
            '                                           config.params.ds1302.utcOffsetMinutes);'
        if drafted.count(fixed_marker) != 1:
            raise AssertionError("RTC fixed serializer marker changed")
        drafted = drafted.replace(fixed_marker, fixed_marker + delegated)
        current = definition(source, "void serializePersistedPeripheral(")
        if current != drafted:
            raise AssertionError("Production serializer changed beyond the reviewed scalar draft; review the frozen oracle")
        parser = definition(source, "bool parsePersistedPeripheral(")
        save = definition(source, "bool PeripheralManager::saveConfiguration(")
        load = definition(source, "bool PeripheralManager::loadConfiguration(")
        serializers = "\n\n".join((current,
            baseline.replace("serializePersistedPeripheral(", "serializePersistedPeripheralOracle(", 1),
            drafted.replace("serializePersistedPeripheral(", "serializePersistedPeripheralDraft(", 1), parser))
        methods = []
        for owner, serializer in (("PeripheralManager", "serializePersistedPeripheral"),
                                  ("OracleManager", "serializePersistedPeripheralOracle"),
                                  ("DraftManager", "serializePersistedPeripheralDraft")):
            if save.count("serializePersistedPeripheral(obj, config);") != 1:
                raise AssertionError("Expected one actual save serializer call")
            methods.append(save.replace("PeripheralManager::", owner + "::", 1).replace(
                "serializePersistedPeripheral(obj, config);", serializer + "(obj, config);"))
            methods.append(load.replace("PeripheralManager::", owner + "::", 1))
        harness = harness_path.read_text(encoding="utf-8")
        for marker, replacement in (("@SERIALIZERS_AND_PARSER@", serializers),
                                    ("@PRODUCTION_SAVE_LOAD_METHODS@", "\n\n".join(methods))):
            if harness.count(marker) != 1:
                raise AssertionError("Expected one fixture marker: " + marker)
            harness = harness.replace(marker, replacement)
        relative = ("include/core/PeripheralConfig.h", "include/core/PeripheralTypes.h",
                    "include/core/FeatureFlags.h", "include/core/ResourceProfile.h",
                    "src/core/PeripheralManagerConfigHelpers.h", "include/network/FixedJsonField.h",
                    "include/core/RtcBootSeedConfig.h",
                    "src/network/FixedJsonField.cpp", "test/mocks/Arduino.h",
                    "test/unit/fixtures/point_history/LittleFS.h")
        cls.inputs = [Path(__file__), source_path, baseline_path, harness_path]
        cls.inputs += [ROOT / path for path in relative]
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.directory = tempfile.TemporaryDirectory(prefix="fastbee-persistence-roundtrip-")
        cls.addClassCleanup(cls.directory.cleanup)
        temporary = Path(cls.directory.name)
        translation_unit = temporary / "roundtrip.cpp"
        translation_unit.write_text(harness, encoding="utf-8")
        cls.binary = temporary / ("roundtrip.exe" if os.name == "nt" else "roundtrip")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        includes = (ROOT / "test/unit/fixtures/point_history", ROOT / "test/mocks",
                    ROOT / "include", ROOT / "src/core", library)
        # Match the existing persistence-extraction fixture: warnings remain
        # enabled without promoting unchanged production signedness warnings.
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-DUNIT_TEST",
                   "-DCONFIG_IDF_TARGET_ESP32S3=1", "-DFASTBEE_TEST_NEEDS_STRLCPY=1",
                   "-DARDUINOJSON_ENABLE_STD_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   *["-I" + str(path) for path in includes], str(translation_unit),
                   str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", timeout=120, env=cls.environment)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("Persistence verification inputs changed during the run")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("serializer-values", "serializer-oom", "save-load", "save-faults", "save-oom", "load-faults", "rtc-boot-parser"):
    setattr(PeripheralPersistenceRoundtripTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
