"""Real ArduinoJson two/three-level field differential; no device accesses."""
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

class FixedJsonNestedTests(unittest.TestCase):
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
        if "fixedNestedJsonFieldForWrite" not in helper.read_text(encoding="utf-8"):
            raise AssertionError("Nested production helper must be integrated before execution")
        fixture = HERE / "cpp/fixed_json_nested_harness.cpp"
        if not fixture.is_file():
            fixture = HERE / "fixed_json_nested_harness.cpp"
        prelude = SUPPORT / "test/unit/cpp/fixed_json_document_prelude.h"
        roots = (ROOT / "include", SUPPORT / "test/unit/cpp", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), fixture, helper, helper_header, prelude,
                 SUPPORT / "test/unit/cpp/document_json_probe.h", SUPPORT / "test/mocks/Arduino.h")
        cls.inputs = sorted(local_includes(seeds, roots) |
                            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-nested-field-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / ("nested.exe" if os.name == "nt" else "nested")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_STD_STRING=1",
                   *["-I" + str(path) for path in roots], "-include", str(prelude),
                   str(fixture), str(helper), "-o", str(cls.binary)]
        compiled = subprocess.run(command, text=True, capture_output=True, encoding="utf-8",
                                  errors="replace", timeout=120, env=cls.environment)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    @classmethod
    def tearDownClass(cls):
        current = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if current != cls.hashes:
            raise AssertionError("Nested helper test inputs changed during execution")

    def check(self, mode, kind, depth):
        result = subprocess.run([str(self.binary), mode, kind, depth], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + " ".join((mode, kind, depth)), result.stdout)

for _mode in ("roots", "allocation-faults", "ownership-lookup", "rhs", "exceptions", "aliasing",
              "const-array-view", "deferred-termination", "const-array-rhs"):
    for _kind in ("document", "object"):
        for _depth in ("2", "3"):
            setattr(FixedJsonNestedTests, "test_" + "_".join((_mode.replace("-", "_"), _kind, _depth)),
                    lambda self, mode=_mode, kind=_kind, depth=_depth: self.check(mode, kind, depth))

if __name__ == "__main__":
    unittest.main()
