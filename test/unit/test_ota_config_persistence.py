"""Exercise production OTA-only parameter patching and filtered persistence reads."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class OtaConfigPersistenceTests(unittest.TestCase):
    def test_settings_roundtrip_preserves_mqtt_modbus_and_unknown_members(self):
        source = (ROOT / "src/network/handlers/ProtocolRouteHandler.cpp").read_text(encoding="utf-8")
        start = source.index("    bool updateModbusRtu =")
        end = source.index("    bool clientIdGenerated", start)
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        self.assertTrue(Path(compiler).exists(), "C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-ota-config-") as directory:
            temporary = Path(directory)
            (temporary / "production_ota_patch.inc").write_text(source[start:end], encoding="utf-8")
            executable = temporary / "ota-config.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                 "-DUSE_NATIVE_STDLIB=1", f"-I{temporary}",
                 f"-I{ROOT / 'test/unit/cpp/ota_config_mocks'}", f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                 f"-I{ROOT / '.pio/libdeps/native/ArduinoJson/src'}",
                 str(ROOT / "test/unit/cpp/ota_config_persistence_harness.cpp"),
                 str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15, env=environment)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
