"""Actual SD JSON serializers against their frozen pre-sharing definitions.

Extracts only the batch/status functions; SDK/storage/scheduler are not executed.
Allocator, String fault probes, and C++ extraction are shared existing fixtures.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
# Also supports direct invocation and staged artifact tests before consolidation.
sys.path.insert(0, str(SUPPORT / "test/unit"))
from test_periph_exec_fixed_json import definition, local_includes
from test_fixed_json_document import string_probe_mock

SIGNATURES = ("size_t sdRecordBatch(", "String sdRecordJson(")


def scalar_draft(source, expected):
    converted, count = re.subn(r'doc\["([^"\n]+)"\] = ([\s\S]*?);',
        lambda item: 'FastBee::setFixedJsonField(doc, "' + item[1] + '", ' + item[2] + ');', source)
    if count != expected:
        raise AssertionError(f"SD literal assignment count changed: {count} != {expected}")
    return converted


def renamed(functions, suffix):
    return "\n\n".join(body.replace(name, name[:-1] + suffix + "(", 1)
                       for body, name in zip(functions, SIGNATURES))


class SdRecorderFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered ArduinoJson headers required")
        source_path = ROOT / "src/core/PeriphExecSdRecorder.cpp"
        oracle_path = HERE / "cpp/sd_recorder_fixed_json_oracle.inc"
        harness_path = HERE / "cpp/sd_recorder_fixed_json_harness.cpp.in"
        source = source_path.read_text(encoding="utf-8")
        oracle = oracle_path.read_text(encoding="utf-8")
        old = [definition(oracle, signature) for signature in SIGNATURES]
        draft = [scalar_draft(body, count) for body, count in zip(old, (15, 13))]
        current = [definition(source, signature) for signature in SIGNATURES]
        for actual, baseline, proposed in zip(current, old, draft):
            if actual not in (baseline, proposed):
                raise AssertionError("SD serializer changed beyond the reviewed fixed-field rewrite")
        program = harness_path.read_text(encoding="utf-8")
        if program.count("@SD_SERIALIZERS@") != 1:
            raise AssertionError("Expected one SD serializer marker")
        program = program.replace("@SD_SERIALIZERS@", "\n\n".join(
            renamed(functions, suffix) for functions, suffix in
            ((old, "Oracle"), (current, "Current"), (draft, "Draft"))))
        helper = ROOT / "src/network/FixedJsonField.cpp"
        prelude = SUPPORT / "test/unit/cpp/fixed_json_document_prelude.h"
        mock = SUPPORT / "test/mocks/Arduino.h"
        roots = (HERE / "cpp", SUPPORT / "test/unit/cpp", ROOT / "include",
                 SUPPORT / "include", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), source_path, oracle_path, harness_path, helper, prelude, mock,
                 ROOT / "include/core/SdRecorderRuntime.h",
                 ROOT / "include/network/FixedJsonField.h",
                 SUPPORT / "test/unit/cpp/document_json_probe.h",
                 SUPPORT / "test/unit/test_periph_exec_fixed_json.py",
                 SUPPORT / "test/unit/test_fixed_json_document.py")
        cls.inputs = sorted(local_includes(seeds, roots) |
                            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-sd-fixed-json-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        (temporary / "Arduino.h").write_text(string_probe_mock(mock.read_text(encoding="utf-8")),
                                           encoding="utf-8")
        cpp = temporary / "sd-fixed-json.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.binary = temporary / ("sd-fixed-json.exe" if os.name == "nt" else "sd-fixed-json")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_STD_STRING=1", "-I" + str(temporary),
                   *["-I" + str(path) for path in roots], "-include", str(prelude),
                   str(cpp), str(helper), "-o", str(cls.binary)]
        result = subprocess.run(command, text=True, capture_output=True, encoding="utf-8",
                                errors="replace", timeout=120, env=cls.environment)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("SD JSON verification inputs changed during execution")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("batch-values", "batch-boundaries", "batch-oom", "status-values", "status-oom", "status-string-faults"):
    setattr(SdRecorderFixedJsonTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
