"""Actual panel response bodies versus frozen originals, including JSON OOM.

Prepared for the consolidated gate. No device, serial, display or firmware access.
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
CPP = HERE if (HERE / "screen_response_fixed_json_harness.cpp").is_file() else ROOT / "test/unit/cpp"
sys.path.insert(0, str(ROOT / "test/unit"))
from test_sensor_publication import definition

SIGNATURES = (
    ("void appendPanelClock(", 4),
    ("void ScreenPanelManager::appendCapabilities(", 33),
    ("void ScreenPanelManager::appendApplication(", 13),
    ("void ScreenPanelManager::appendState(", 69),
)


class ScreenResponseFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE",
                       SUPPORT / ".pio/libdeps/native/ArduinoJson/src"))
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson required")
        cls.inputs = [Path(__file__), CPP / "screen_response_fixed_json_harness.cpp",
            CPP / "screen_response_oracle.inc", ROOT / "test/unit/cpp/serializer_trace_allocator.h",
            ROOT / "test/unit/test_sensor_publication.py", SUPPORT / "test/mocks/Arduino.h",
            ROOT / "include/core/ScreenPanelManager.h", ROOT / "include/screen/ScreenRuntime.h",
            ROOT / "src/core/ScreenPanelManager.cpp", ROOT / "include/network/FixedJsonField.h",
            ROOT / "src/network/FixedJsonField.cpp"]
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.addClassCleanup(cls.check_inputs)
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-screen-response-")
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        source = (ROOT / "src/core/ScreenPanelManager.cpp").read_text(encoding="utf-8")
        oracle = (CPP / "screen_response_oracle.inc").read_text(encoding="utf-8")
        actual = []
        for signature, count in SIGNATURES:
            current = definition(source, signature)
            original = definition(oracle, signature)
            reversed_body, replacements = re.subn(
                r'FastBee::setFixedJsonField\((clock|out|disabledReasons|state|item|seriesItem), "([^"]+)", ([\s\S]*?)\);',
                lambda m: m[1] + '["' + m[2] + '"] = ' + m[3] + ';', current)
            if replacements != count or reversed_body != original:
                raise AssertionError("Body changed beyond reviewed fixed scalar assignments: " + signature)
            actual.append(current)
        (folder / "screen_response_actual.inc").write_text("\n\n".join(actual), encoding="utf-8")
        original = oracle.replace("ScreenPanelManager::", "OracleScreenPanelManager::")
        original = original.replace("appendPanelClock(", "appendPanelClockOracle(")
        (folder / "screen_response_original.inc").write_text(original, encoding="utf-8")
        header = (ROOT / "include/core/ScreenPanelManager.h").read_text(encoding="utf-8")
        types = header[header.index("enum class ScreenCardKind"):header.index("struct ScreenPanelSnapshotCache;")]
        poison = "std::array<ScreenPanelCardSnapshot, MAX_CARDS> cards{};"
        if types.count(poison) != 1:
            raise AssertionError("Snapshot ownership seam changed")
        types = types.replace(poison, poison +
            "\n    ~ScreenPanelSnapshot() { std::memset(static_cast<void*>(cards.data()), 0, sizeof(cards)); }")
        runtime = (ROOT / "include/screen/ScreenRuntime.h").read_text(encoding="utf-8")
        types += "\nnamespace FastBee { namespace Screen {\n"
        types += definition(runtime, "struct ScreenSystemActionCapabilities") + ";\n}}\n"
        (folder / "screen_response_types.inc").write_text(types, encoding="utf-8")
        (folder / "screen_quality.inc").write_text(definition(source, "const char* qualityName("), encoding="utf-8")
        # Keep Arduino mock allocator macros out of both the harness and the
        # real helper translation unit, with otherwise identical AJ flags.
        (folder / "fixture_prefix.h").write_text(
            "#include <Arduino.h>\n#undef malloc\n#undef free\n", encoding="utf-8")
        cls.env = dict(os.environ)
        cls.env["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.env.get("PATH", "")
        cls.binaries = {}
        for screen_ui in (0, 1):
            binary = folder / ("screen-" + str(screen_ui) + ".exe")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                "-DFASTBEE_ENABLE_SCREEN_UI=" + str(screen_ui),
                "-DFASTBEE_ENABLE_MODBUS=" + str(screen_ui),
                "-I" + str(folder), "-I" + str(CPP), "-I" + str(ROOT / "test/unit/cpp"),
                "-I" + str(SUPPORT / "test/mocks"), "-I" + str(ROOT / "include"),
                "-I" + str(library), "-include", str(folder / "fixture_prefix.h"),
                str(CPP / "screen_response_fixed_json_harness.cpp"),
                str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(binary)]
            built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                errors="replace", timeout=120, env=cls.env)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries[screen_ui] = binary

    @classmethod
    def check_inputs(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError("Panel response production/oracle/library/fixture input drift")

    def check(self, mode):
        for screen_ui, binary in self.binaries.items():
            with self.subTest(screen_ui=screen_ui):
                run = subprocess.run([str(binary), mode], capture_output=True, text=True,
                    encoding="utf-8", errors="replace", timeout=180, env=self.env)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
                self.assertIn("PASS " + mode, run.stdout)


for _mode in ("clock", "capabilities", "application", "state"):
    setattr(ScreenResponseFixedJsonTests, "test_" + _mode,
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
