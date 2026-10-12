"""Actual ArduinoJson String-reader versus explicit const-bytes regression.

Run by the central gate; these are host parser/library semantics, not firmware
size or physical acceptance. The production change preserves each caller's
checks and only passes String::c_str()/length() explicitly.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))


class JsonStringReaderDedupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise unittest.SkipTest("C++17 compiler required")
        library = SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise AssertionError("Registered native ArduinoJson dependency required")
        cls.source = ROOT / "test/unit/cpp/json_string_reader_dedup_harness.cpp"
        cls.inputs = [Path(__file__).resolve(), cls.source, SUPPORT / "test/mocks/Arduino.h"]
        cls.inputs.extend(sorted(library.rglob("*.hpp")))
        cls.inputs.extend(sorted(library.rglob("*.h")))
        cls.before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-json-string-reader-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "json-string-reader.exe"
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        build = subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-I" + str(SUPPORT / "test/mocks"), "-I" + str(library),
            str(cls.source), "-o", str(cls.binary)],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=60, env=cls.environment)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.before:
            raise AssertionError("JSON reader fixture or actual library changed during execution")

    def run_mode(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True,
            text=True, encoding="utf-8", errors="replace", timeout=15, env=self.environment)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_json_types_errors_escapes_nul_and_trailing_behavior(self):
        self.run_mode("syntax")

    def test_exact_byte_length_unsigned_reads_and_original_nesting_limit(self):
        self.run_mode("length-nesting")

    def test_keys_values_and_embedded_nul_remain_owned_after_source_release(self):
        self.run_mode("ownership")

    def test_allocation_failure_trace_partial_document_and_oom_match(self):
        self.run_mode("allocation-failures")


if __name__ == "__main__":
    unittest.main()
