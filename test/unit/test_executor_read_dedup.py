"""Compare shared executor read helpers to frozen original blocks, including OOM."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class ExecutorReadDedupTests(unittest.TestCase):
    def test_original_blocks_and_shared_helpers(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        headers = sorted((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not compiler or not headers:
            raise RuntimeError("C++17 compiler and actual ArduinoJson headers required")
        source_path = ROOT / "src/core/PeriphExecExecutor.cpp"
        oracle_path = ROOT / "test/unit/cpp/executor_read_dedup_oracle.inc"
        harness_path = ROOT / "test/unit/cpp/executor_read_dedup_harness.cpp.in"
        inputs = [source_path, oracle_path, harness_path, Path(__file__), ROOT / "test/mocks/Arduino.h"]
        inputs += sorted(headers[0].parent.rglob("*.h")) + sorted(headers[0].parent.rglob("*.hpp"))
        before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs]
        source = source_path.read_text(encoding="utf-8")
        current = "\n".join(cpp_function(source, signature) for signature in (
            "bool loadSensorReadSnapshot(", "void invalidateSensorReadDisplay(", "bool appendModbusPollReadings("))
        oracle = oracle_path.read_text(encoding="utf-8")
        self.assertEqual('419e98a92b6376345533219324de34d211fca63e97abacaa3258d8a5fedd4c58',
                         hashlib.sha256(oracle_path.read_bytes()).hexdigest(),
                         'Frozen original executor blocks changed')
        functions = oracle + "\n" + current
        program = harness_path.read_text(encoding="utf-8")
        if program.count("// @functions@") != 1:
            raise AssertionError("Harness insertion marker changed")
        with tempfile.TemporaryDirectory(prefix="fastbee-executor-read-dedup-") as directory:
            folder = Path(directory)
            cpp = folder / "compare.cpp"
            cpp.write_text(program.replace("// @functions@", functions), encoding="utf-8")
            binary = folder / ("compare.exe" if os.name == "nt" else "compare")
            built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                "-I" + str(ROOT / "test/mocks"), "-I" + str(headers[0].parent),
                str(cpp), "-o", str(binary)], capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=90)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("executor read dedup comparisons passed: 1108", result.stdout)
        self.assertEqual(before, [hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs],
                         "Executor dedup comparison inputs changed during execution")


if __name__ == "__main__":
    unittest.main()

