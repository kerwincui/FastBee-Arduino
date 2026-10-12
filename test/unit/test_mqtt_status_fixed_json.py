"""Full MQTT status response versus frozen original, including real AJ OOM.

Prepared for the consolidated gate. No devices, transport or live queue access.
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

ROOT = Path(os.environ.get("FASTBEE_TEST_ROOT", next(
    path for path in Path(__file__).resolve().parents if (path / "platformio.ini").is_file())))
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
HERE = Path(__file__).resolve().parent
CPP = HERE if (HERE / "mqtt_status_fixed_json_harness.cpp").is_file() else ROOT / "test/unit/cpp"
sys.path.insert(0, str(ROOT / "test/unit"))
from test_sensor_publication import definition


class MqttStatusFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE",
                       SUPPORT / ".pio/libdeps/native/ArduinoJson/src"))
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson required")
        cls.inputs = [Path(__file__), CPP / "mqtt_status_fixed_json_harness.cpp",
            CPP / "mqtt_status_oracle.inc", ROOT / "test/unit/cpp/serializer_trace_allocator.h",
            ROOT / "test/unit/test_sensor_publication.py", ROOT / "include/protocols/MQTTClient.h",
            ROOT / "include/protocols/MqttReportRetirement.h", ROOT / "include/protocols/MqttReportQueueStatus.h",
            ROOT / "include/network/FixedJsonField.h", ROOT / "src/network/FixedJsonField.cpp"]
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.addClassCleanup(cls.check_inputs)
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-mqtt-status-")
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = (ROOT / "include/protocols/MqttReportQueueStatus.h").read_text(encoding="utf-8")
        oracle = (CPP / "mqtt_status_oracle.inc").read_text(encoding="utf-8")
        signature = "inline void addMqttReportQueueStatus("
        current = definition(source, signature)
        original = definition(oracle, signature)
        reversed_body, count = re.subn(
            r'FastBee::setFixedJsonField\((queue|retirement), "([^"]+)", ([^\n]+?)\);',
            lambda match: match[1] + '["' + match[2] + '"] = ' + match[3] + ';', current)
        if count != 30 or reversed_body != original:
            raise AssertionError("MQTT serializer changed beyond 30 reviewed fixed scalar writes")
        (folder / "mqtt_status_original.inc").write_text(
            original.replace("addMqttReportQueueStatus(", "addMqttReportQueueStatusOriginal("), encoding="utf-8")
        header = (ROOT / "include/protocols/MQTTClient.h").read_text(encoding="utf-8")
        slots = re.findall(r"^static constexpr uint8_t MQTT_REPORT_SLOTS = [^\n]+", header, re.M)
        if len(slots) != 1:
            raise AssertionError("MQTT slot definition seam changed")
        types = slots[0] + "\nclass MQTTClient {\npublic:\n"
        types += definition(header, "struct QueueStats") + ";\n"
        types += definition(header, "struct QueueSnapshot") + ";\n"
        types += "static FastBee::MqttReportRetirement::Snapshot getReportRetirementSnapshot();\n};\n"
        (folder / "mqtt_status_types.inc").write_text(types, encoding="utf-8")
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.env.get("PATH", "")
        cls.binary = folder / "mqtt-status.exe"
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-I" + str(folder), "-I" + str(ROOT / "test/unit/cpp"),
            "-I" + str(ROOT / "include"), "-I" + str(library),
            str(CPP / "mqtt_status_fixed_json_harness.cpp"),
            str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
            errors="replace", timeout=120, env=cls.env)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def check_inputs(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError("MQTT status production/oracle/library/fixture input drift")

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=90, env=self.env)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertIn("PASS " + mode, run.stdout)


for _mode in ("present", "absent", "reused", "unbound"):
    setattr(MqttStatusFixedJsonTests, "test_" + _mode,
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()

