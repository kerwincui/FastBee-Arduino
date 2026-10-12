"""Actual status serializers against frozen pre-sharing ArduinoJson bodies."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = next(path for path in Path(__file__).resolve().parents
            if (path / "platformio.ini").is_file())
HERE = Path(__file__).resolve().parent
CPP = HERE if (HERE / "status_serializer_harness.cpp").is_file() else ROOT / "test/unit/cpp"

class StatusFixedJsonSerializerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("C++17 compiler required for actual serializer coverage")
        json_root = os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE")
        if not json_root:
            headers = sorted((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
            if not headers:
                raise RuntimeError("Installed ArduinoJson required")
            json_root = str(headers[0].parent)
        cls.inputs = [ROOT / name for name in (
            "include/network/TaskStackSnapshotJson.h", "include/systems/TaskStackSnapshot.h",
            "include/network/SDCardStatusJson.h", "include/peripherals/SDCardManager.h",
            "include/network/SystemTimeStatus.h", "include/utils/SystemTimeQuality.h",
            "include/utils/SystemTimeZone.h", "include/network/FixedJsonField.h",
            "src/network/FixedJsonField.cpp", "test/mocks/Arduino.h")]
        cls.inputs += [CPP / "status_serializer_harness.cpp",
                       CPP / "status_serializer_oracle.inc", Path(__file__)]
        cls.before = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.folder = tempfile.TemporaryDirectory(prefix="fastbee-status-fixed-json-")
        cls.addClassCleanup(cls.folder.cleanup)
        cls.addClassCleanup(cls.verify_inputs)
        cls.binary = Path(cls.folder.name) / "status.exe"
        # Keep the pre-sharing oracle unchanged. Extend only the reviewed RTC
        # schema field with ordinary assignment, independently of the helper.
        oracle = (CPP / "status_serializer_oracle.inc").read_text(encoding="utf-8")
        marker = '    data["timeQuality"] = status.quality;'
        if oracle.count(marker) != 1:
            raise AssertionError("RTC schema oracle marker changed")
        oracle = oracle.replace(marker, marker + '\n    data["rtcBootStatus"] = status.rtcBootStatus;')
        folder = Path(cls.folder.name)
        (folder / "status_serializer_rtc_oracle.inc").write_text(oracle, encoding="utf-8")
        harness = (CPP / "status_serializer_harness.cpp").read_text(encoding="utf-8")
        include = '#include "status_serializer_oracle.inc"'
        if harness.count(include) != 1:
            raise AssertionError("Expected one frozen oracle include")
        generated = folder / "status.cpp"
        generated.write_text(harness.replace(include, '#include "status_serializer_rtc_oracle.inc"'),
                             encoding="utf-8")
        compiled = subprocess.run([
            compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-I" + str(ROOT / "test/mocks"), "-I" + str(ROOT / "include"),
            "-I" + json_root, str(generated),
            str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(cls.binary)],
            capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=90)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    @classmethod
    def verify_inputs(cls):
        after = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        if cls.before != after:
            raise AssertionError("Status serializer inputs changed during tests")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_task_capture_states_capacities_bytes_and_allocation_faults(self):
        self.check("task")

    def test_sd_owner_quality_history_bytes_and_allocation_faults(self):
        self.check("sd")

    def test_time_provenance_local_array_ownership_and_allocation_faults(self):
        self.check("time")

if __name__ == "__main__":
    unittest.main()
