"""Exact JsonDocument member-proxy differential and real pending-status JSON.

No device, networking, or SDK build is performed. ArduinoJson is the installed
library, the helper is a separate real production TU, String/RTOS are host mocks.
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


def local_includes(seeds, roots):
    pending, seen = list(seeds), set()
    while pending:
        path = pending.pop().resolve()
        if path in seen or not path.is_file():
            continue
        seen.add(path)
        for name in re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', path.read_text(encoding="utf-8"), re.M):
            for base in (path.parent, *roots):
                candidate = base / name
                if candidate.is_file():
                    pending.append(candidate)
                    break
    return seen


def draft_status(source):
    updated = source.replace('#include "core/PendingReportQueue.h"',
        '#include "core/PendingReportQueue.h"\n#include "network/FixedJsonField.h"', 1)
    updated, count = re.subn(r'doc\["([^"]+)"\] = ([\s\S]*?);',
        lambda match: 'setFixedJsonField(doc, "' + match[1] + '", ' + match[2] + ');', updated)
    if count != 24:
        raise AssertionError("Pending status literal assignments changed: " + str(count))
    return updated


def status_function(source, suffix):
    signature = "inline String pendingReportStatusJson(const PendingReportQueue::Status& status)"
    if source.count(signature) != 1 or source.count("JsonDocument doc;") != 1:
        raise AssertionError("Expected one exact status writer and document")
    # The only behavior seam is a real AJ allocator at construction. Every
    # assignment, condition, fallback and String return is retained verbatim.
    source = source.replace(signature,
        "inline String pendingReportStatusJson" + suffix +
        "(const PendingReportQueue::Status& status, DocumentProbeAllocator& statusAllocator)", 1)
    source = source.replace("JsonDocument doc;", "JsonDocument doc(&statusAllocator);", 1)
    return "\n".join(line for line in source.splitlines()
                     if not line.startswith("#include") and line != "#pragma once")


def string_probe_mock(source):
    declaration = """
struct StatusStringProbe {
    static inline unsigned mode = 0, reserveCalls = 0, concatCalls = 0;
    static void reset(unsigned selected) {
        mode = selected; reserveCalls = concatCalls = 0;
    }
};
"""
    if source.count("class String {") != 1:
        raise AssertionError("String mock declaration changed")
    source = source.replace("class String {", declaration + "\nclass String {", 1)
    reserve = "bool reserve(size_t size) { _value.reserve(size); return true; }"
    if source.count(reserve) != 1:
        raise AssertionError("String reserve mock changed")
    source = source.replace(reserve, """bool reserve(size_t size) {
        ++StatusStringProbe::reserveCalls;
        if (StatusStringProbe::mode == 1) return false;
        _value.reserve(size); return true;
    }""", 1)
    marker = "void clear() { _value.clear(); }"
    if source.count(marker) != 1:
        raise AssertionError("String clear mock changed")
    source = source.replace(marker, marker + """
    bool concat(const char* value) {
        ++StatusStringProbe::concatCalls;
        if (StatusStringProbe::mode == 2) return false;
        const size_t size = value ? std::strlen(value) : 0;
        if (StatusStringProbe::mode == 3) {
            if (size) _value.append(value, size / 2);
            return true;
        }
        if (size) _value.append(value, size);
        return StatusStringProbe::mode != 4;
    }
""", 1)
    return source


class FixedJsonDocumentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered ArduinoJson headers required")
        helper = ROOT / "src/network/FixedJsonField.cpp"
        helper_header = ROOT / "include/network/FixedJsonField.h"
        status_path = ROOT / "include/network/PendingReportStatus.h"
        oracle_path = HERE / "cpp/pending_report_status_before.inc"
        mock = SUPPORT / "test/mocks/Arduino.h"
        prelude = HERE / "cpp/fixed_json_document_prelude.h"
        shared_probe = HERE / "cpp/document_json_probe.h"
        root_harness = HERE / "cpp/fixed_json_document_harness.cpp"
        status_harness = HERE / "cpp/pending_report_status_harness.cpp.in"
        oracle = oracle_path.read_text(encoding="utf-8")
        draft = draft_status(oracle)
        current = status_path.read_text(encoding="utf-8")
        if current.strip() != draft.strip():
            raise AssertionError("Production Pending24 must match the reviewed draft exactly")
        program = status_harness.read_text(encoding="utf-8")
        if program.count("@STATUS_SERIALIZERS@") != 1:
            raise AssertionError("Expected one serializer marker")
        program = program.replace("@STATUS_SERIALIZERS@", "\n\n".join(
            status_function(source, suffix) for source, suffix in
            ((oracle, "Oracle"), (current, "Current"), (draft, "Draft"))))
        roots = (HERE / "cpp", ROOT / "include", SUPPORT / "test/unit/cpp/pending_report_mocks",
                 SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), helper, helper_header, status_path, oracle_path, mock, prelude,
                 shared_probe, root_harness, status_harness)
        cls.inputs = sorted(local_includes(seeds, roots) |
            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-json-document-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        (temporary / "Arduino.h").write_text(string_probe_mock(mock.read_text(encoding="utf-8")),
                                           encoding="utf-8")
        status_cpp = temporary / "pending-status.cpp"
        status_cpp.write_text(program, encoding="utf-8")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        cls.binaries = {}
        for label, cpp in (("root", root_harness), ("pending", status_cpp)):
            binary = temporary / (label + (".exe" if os.name == "nt" else ""))
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                "-DARDUINOJSON_ENABLE_STD_STRING=1", "-I" + str(temporary),
                *["-I" + str(path) for path in roots], "-include", str(prelude),
                str(cpp), str(helper), "-o", str(binary)]
            built = subprocess.run(command, text=True, capture_output=True, encoding="utf-8",
                                   errors="replace", timeout=120, env=cls.environment)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries[label] = binary

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("Document field test inputs changed during execution")

    def check(self, mode):
        binary = self.binaries["pending" if mode.startswith("pending-") else "root"]
        result = subprocess.run([str(binary), mode], text=True, capture_output=True, encoding="utf-8",
                                errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("roots", "allocation-faults", "lookup", "rhs", "exceptions", "aliasing",
              "pending-values", "pending-allocation-faults", "pending-string-faults",
              "scalar-widths", "scalar-allocation-faults"):
    setattr(FixedJsonDocumentTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
