"""Real PointHistory fixed-field serializers against frozen pre-sharing bodies.

The five changed method bodies are compiled completely. Configuration storage,
lifecycle and RTOS are explicit seams; the existing full persistence regression
separately compiles the entire production service and byte-backed faulty storage.
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
sys.path.insert(0, str(SUPPORT / "test/unit"))
from test_periph_exec_fixed_json import definition, local_includes
from test_fixed_json_document import string_probe_mock

SIGNATURES = ("bool PointHistory::configure(", "void PointHistory::appendSeriesJson(",
              "void PointHistory::appendConfigJson(", "void PointHistory::appendStatusJson(",
              "bool PointHistory::writeHistorySnapshot(")
COUNTS = (3, 18, 5, 8, 14)


def scalar_draft(source, expected):
    result, count = re.subn(r'(doc|out|source|item)\["([^"\n]+)"\] = ([^;\n]+);',
        lambda match: f'FastBee::setFixedJsonField({match[1]}, "{match[2]}", {match[3]});', source)
    if count != expected:
        raise AssertionError(f"Reviewed PointHistory assignment count changed: {count} != {expected}")
    return result


def renamed(functions, suffix):
    return "\n\n".join(body.replace("PointHistory::", "Point" + suffix + "::", 1) for body in functions)


def point_string_mock(source):
    result = string_probe_mock(source)
    old = "bool concat(const char* value, unsigned int length) { if (!value && length) return false; if (length) _value.append(value, length); return true; }"
    if result.count(old) != 1:
        raise AssertionError("Length-aware String concat seam changed")
    return result.replace(old, """bool concat(const char* value, unsigned int length) {
        ++StatusStringProbe::concatCalls;
        if ((!value && length) || StatusStringProbe::mode == 2) return false;
        if (StatusStringProbe::mode == 3) {
            if (length) _value.append(value, length / 2);
            return true;
        }
        if (length) _value.append(value, length);
        return StatusStringProbe::mode != 4;
    }""", 1)


class PointHistoryFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson headers required")
        source_path = ROOT / "src/core/PointHistory.cpp"
        oracle_path = HERE / "cpp/point_history_fixed_json_oracle.inc"
        harness_path = HERE / "cpp/point_history_fixed_json_harness.cpp.in"
        source = source_path.read_text(encoding="utf-8")
        oracle = oracle_path.read_text(encoding="utf-8")
        old = [definition(oracle, signature) for signature in SIGNATURES]
        proposed = [scalar_draft(body, count) for body, count in zip(old, COUNTS)]
        current = [definition(source, signature) for signature in SIGNATURES]
        for actual, baseline, draft in zip(current, old, proposed):
            if actual not in (baseline, draft):
                raise AssertionError("PointHistory changed beyond the reviewed 48 fixed-field writes")
        program = harness_path.read_text(encoding="utf-8")
        replacements = {
            "@PRODUCTION_HISTORY_LOCK@": definition(source, "class HistoryLock") + ";",
            "@PRODUCTION_GET_SERIES@": definition(source, "bool PointHistory::getSeries(").replace(
                "PointHistory::", "PointFixture::", 1),
            "@PRODUCTION_VALID_SOURCE@": definition(source, "bool PointHistory::validSourceRef(").replace(
                "PointHistory::", "PointFixture::", 1),
            "@POINT_WRITERS@": "\n\n".join(renamed(functions, suffix) for functions, suffix in
                ((old, "Oracle"), (current, "Current"), (proposed, "Draft")))
        }
        for marker, text in replacements.items():
            if program.count(marker) != 1:
                raise AssertionError("Expected one marker: " + marker)
            program = program.replace(marker, text)
        helper = ROOT / "src/network/FixedJsonField.cpp"
        prelude = SUPPORT / "test/unit/cpp/fixed_json_document_prelude.h"
        mock = SUPPORT / "test/mocks/Arduino.h"
        roots = (HERE / "cpp", SUPPORT / "test/unit/cpp", ROOT / "include",
                 SUPPORT / "include", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), source_path, oracle_path, harness_path, helper, prelude, mock,
                 ROOT / "include/core/PointHistory.h", ROOT / "include/network/FixedJsonField.h",
                 ROOT / "include/utils/CheckedText.h", SUPPORT / "test/unit/cpp/document_json_probe.h",
                 SUPPORT / "test/unit/test_periph_exec_fixed_json.py",
                 SUPPORT / "test/unit/test_fixed_json_document.py")
        cls.inputs = sorted(local_includes(seeds, roots) |
                            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}
        cls.directory = tempfile.TemporaryDirectory(prefix="fastbee-point-fixed-json-")
        cls.addClassCleanup(cls.directory.cleanup)
        temporary = Path(cls.directory.name)
        (temporary / "Arduino.h").write_text(point_string_mock(mock.read_text(encoding="utf-8")),
                                           encoding="utf-8")
        cpp = temporary / "point-fixed-json.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.binary = temporary / ("point-fixed-json.exe" if os.name == "nt" else "point-fixed-json")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
            "-DFASTBEE_ENABLE_POINT_HISTORY=1", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
            "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-I" + str(temporary), *["-I" + str(p) for p in roots], "-include", str(prelude),
            str(cpp), str(helper), "-o", str(cls.binary)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120, env=cls.environment)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}:
            raise AssertionError("PointHistory JSON inputs changed during validation")

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                             encoding="utf-8", errors="replace", timeout=180, env=self.environment)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn("PASS " + mode, ran.stdout)


for _mode in ("series-values", "series-oom", "config-values", "config-oom",
              "status-values", "status-oom", "history-values", "history-oom",
              "configure-values", "configure-oom", "configure-string-faults"):
    setattr(PointHistoryFixedJsonTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
