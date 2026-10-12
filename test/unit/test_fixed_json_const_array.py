"""Real ArduinoJson differential for the narrow const-array sharing candidate.

Parent integration only: no result is implied by this unexecuted draft.
Place this file in test/unit and the two C++ files in test/unit/cpp.
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
CPP = HERE if (HERE / "fixed_json_const_array_harness.cpp").is_file() else HERE / "cpp"


def local_inputs(seeds, roots):
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


class FixedJsonConstArrayTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("Registered C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson required")
        header = ROOT / "include/network/FixedJsonField.h"
        source = ROOT / "src/network/FixedJsonField.cpp"
        header_text = header.read_text(encoding="utf-8")
        source_text = source.read_text(encoding="utf-8")
        if ("JsonString(value, sourceCapacityMinusOne, true)" not in source_text or
                header_text.count("std::extent<Source>::value - 1") != 6 or
                "JsonString(value, true)" in source_text):
            raise RuntimeError("All six capacity-preserving field overloads must be integrated before this regression runs")
        prelude = ROOT / "test/unit/cpp/fixed_json_document_prelude.h"
        harness = CPP / "fixed_json_const_array_harness.cpp"
        roots = (CPP, ROOT / "test/unit/cpp", ROOT / "include", SUPPORT / "test/mocks", library)
        cls.inputs = sorted(local_inputs((Path(__file__), header, source, harness, prelude), roots) |
                            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-const-array-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.addClassCleanup(cls.verify_inputs)
        cls.binary = Path(cls.temporary.name) / "const-array.exe"
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        built = subprocess.run([
            compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-DARDUINOJSON_ENABLE_STD_STRING=1", *["-I" + str(root) for root in roots],
            "-include", str(prelude), str(harness), str(source), "-o", str(cls.binary)
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
           env=cls.environment)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def verify_inputs(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("Const-array regression inputs changed while running")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("roots-and-values", "all-allocation-faults", "const-view-ownership", "rhs-and-exceptions", "deferred-termination"):
    setattr(FixedJsonConstArrayTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
