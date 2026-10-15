"""Real RTC startup hook/driver/quality plus production form bodies. No device or OS clock."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def definition(source, signature):
    if source.count(signature) != 1:
        raise AssertionError("Expected one definition: " + signature)
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise AssertionError("Unterminated definition")


class RtcBootSeedTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("C++17 compiler required")
        json_root = os.environ.get("FASTBEE_TEST_ARDUINOJSON_INCLUDE")
        if not json_root:
            found = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
            if not found:
                raise RuntimeError("Installed ArduinoJson required")
            json_root = str(found[0].parent)
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-rtc-boot-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        paths = (
            "include/utils/SystemTimeQuality.h", "src/utils/SystemTimeQuality.cpp",
            "include/utils/SystemTimePlatform.h", "include/core/PeripheralConfig.h",
            "include/core/RtcAccess.h", "include/core/RtcBootSeedConfig.h",
            "src/core/PeripheralManagerRtc.cpp", "include/core/PeripheralManager.h",
            "src/core/FastBeeFramework.cpp", "src/peripherals/drivers/DS1302Driver.cpp",
            "src/network/handlers/PeripheralRouteHandler.cpp",
            "src/core/PeripheralRouteMapper.cpp",
            "src/core/PeripheralParamApply.cpp",
            "test/unit/cpp/rtc_boot_seed_harness.cpp",
            "test/unit/cpp/rtc_boot_mocks/core/PeripheralManager.h",
            "test/unit/cpp/rtc_boot_form_harness.cpp.in",
            "web-src/modules/runtime/peripherals.js", "web-src/modules/runtime/peripheral-save.js",
            "test/helpers/rtc-boot-form-web-harness.js", "web-src/pages/modals.html",
            "include/core/PeripheralTypes.h", "include/core/FeatureFlags.h",
            "include/core/AsyncExecTypes.h", "include/core/FeatureInstanceRegistry.h",
            "include/network/FixedJsonField.h", "src/network/FixedJsonField.cpp",
            "include/peripherals/drivers/DS1302Driver.h")
        cls.inputs = [Path(__file__), *[ROOT / p for p in paths]]
        for folder in ("test/unit/cpp/rtc_action_mocks", "test/unit/cpp/rtc_datetime_mocks"):
            cls.inputs += sorted((ROOT / folder).rglob("*.h"))
        cls.inputs += sorted(Path(json_root).rglob("*.h")) + sorted(Path(json_root).rglob("*.hpp"))
        cls.hashes = {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}
        cls.binaries = {}
        for compiled in (0, 1):
            binary = directory / ("boot%d.exe" % compiled)
            includes = ("test/unit/cpp/rtc_boot_mocks", "test/unit/cpp/rtc_action_mocks",
                        "test/unit/cpp/rtc_datetime_mocks", "test/mocks", "test/unit/cpp/mocks", "include")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra",
                       "-DFASTBEE_ENABLE_DS1302=%d" % compiled, "-DUNIT_TEST",
                       "-DCONFIG_IDF_TARGET_ESP32S3=1", "-DFASTBEE_TEST_NEEDS_STRLCPY=1", "-I" + json_root]
            command += ["-I" + str(ROOT / p) for p in includes]
            command += [str(ROOT / p) for p in ("test/unit/cpp/rtc_boot_seed_harness.cpp",
                        "src/core/PeripheralManagerRtc.cpp", "src/utils/SystemTimeQuality.cpp",
                        "src/peripherals/drivers/DS1302Driver.cpp")]
            command += ["-o", str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.binaries[compiled] = binary
        route_paths = (
            "src/core/PeripheralRouteMapper.cpp",
            "src/network/handlers/PeripheralRouteHandler.cpp",
            "src/core/PeripheralParamApply.cpp",
        )
        route = "\n".join((ROOT / path).read_text(encoding="utf-8") for path in route_paths)
        functions = "\n\n".join(definition(route, name) for name in
                               ("void addP1PeripheralParams(", "bool applyP1PeripheralParams("))
        template = (ROOT / "test/unit/cpp/rtc_boot_form_harness.cpp.in").read_text(encoding="utf-8")
        if template.count("@PRODUCTION_P1_FUNCTIONS@") != 1:
            raise AssertionError("Form fixture marker missing")
        source = directory / "form.cpp"
        source.write_text(template.replace("@PRODUCTION_P1_FUNCTIONS@", functions), encoding="utf-8")
        cls.form = directory / "form.exe"
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-DUNIT_TEST",
            "-DCONFIG_IDF_TARGET_ESP32S3=1", "-DFASTBEE_TEST_NEEDS_STRLCPY=1",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
            "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
            "-I" + str(ROOT / "test/mocks"), "-I" + str(ROOT / "include"), "-I" + json_root,
            str(source), str(ROOT / "src/network/FixedJsonField.cpp"), "-o", str(cls.form)],
            capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs}:
            raise AssertionError("RTC startup inputs changed during verification")

    def check(self, case, compiled=1):
        ran = subprocess.run([str(self.binaries[compiled]), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_not_compiled_has_no_io(self):
        self.check("not-compiled", 0)

    def test_real_http_parser_and_serializer(self):
        ran = subprocess.run([str(self.form)], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_actual_form_save_and_restore(self):
        node = shutil.which("node")
        self.assertIsNotNone(node, "Node required")
        ran = subprocess.run([node, str(ROOT / "test/helpers/rtc-boot-form-web-harness.js")],
                             capture_output=True, text=True, encoding="utf-8", timeout=15)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn("RTC boot full form save", ran.stdout)

    def test_real_framework_places_hook_before_network_and_tasks(self):
        source = (ROOT / "src/core/FastBeeFramework.cpp").read_text(encoding="utf-8")
        hook = "PeripheralManager::getInstance().seedSystemClockFromRtcAtBoot();"
        self.assertEqual(source.count(hook), 1)
        start = source.index("PeripheralManager::getInstance().initialize()")
        position = source.index(hook)
        server = source.index("server.reset(new AsyncWebServer(80))", start)
        self.assertLess(start, position)
        self.assertLess(position, server)


for _mode in (
    "applied", "disabled", "peripheral-disabled", "none", "multiple", "valid-wall", "busy", "no-mutex",
    "no-offset", "bad-offset", "runtime-disabled", "suspended", "bad-state", "state-missing", "uninitialized",
    "owner-mismatch", "halted", "invalid-calendar", "invalid-bcd", "early-epoch", "late-epoch", "utc",
    "offset-max", "offset-min", "midnight", "12hour", "mono-unavailable", "source-race", "slow-read",
    "write-failure", "service-disabled", "service-valid", "service-invalid-mono",
    "service-write-failure", "service-new-wall", "service-sntp-race", "service-mono-write",
    "service-existing-source", "service-applied", "service-stale", "service-jump", "service-network",
    "service-range"):
    setattr(RtcBootSeedTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
