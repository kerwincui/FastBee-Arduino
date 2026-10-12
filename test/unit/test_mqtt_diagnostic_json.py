"""MQTT diagnostic refactor compatibility; real serializer/AJ, no device."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MqttDiagnosticJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        headers = sorted((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not compiler or not headers:
            raise RuntimeError("C++17 compiler and installed ArduinoJson required")
        cls.inputs = [ROOT / item for item in (
            "include/protocols/MqttDiagnosticJson.h", "src/protocols/MqttDiagnosticJson.cpp",
            "test/unit/cpp/mqtt_diagnostic_json_harness.cpp.in",
            "test/unit/test_mqtt_diagnostic_json.py")]
        cls.hashes = [hashlib.sha256(item.read_bytes()).hexdigest() for item in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-mqtt-diagnostic-")
        cls.addClassCleanup(cls.temp.cleanup)
        cpp = Path(cls.temp.name) / "diagnostic.cpp"
        cpp.write_text(cls.inputs[2].read_text(encoding="utf-8"), encoding="utf-8")
        cls.binary = Path(cls.temp.name) / ("diagnostic.exe" if os.name == "nt" else "diagnostic")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-I" + str(ROOT / "include"), "-I" + str(headers[0].parent),
            str(cpp), str(cls.inputs[1]), "-o", str(cls.binary)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(item.read_bytes()).hexdigest() for item in cls.inputs]:
            raise AssertionError("Diagnostic inputs changed during verification")

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ("shapes", "reason_precedence", "separate_messages", "ownership",
              "allocation_failures", "nested_allocation_failures", "invalid_and_ok"):
    setattr(MqttDiagnosticJsonTests, "test_" + _case, lambda self, case=_case: self.check(case))

if __name__ == "__main__":
    unittest.main()
