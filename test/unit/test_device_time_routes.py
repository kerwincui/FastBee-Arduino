"""Real device-time route response regression, including old unbound negative.

No network, serial, RTC writes or device operations. This artifact is unexecuted
until the coordinator integrates it and runs the serialized verification.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from test_device_batch_fixed_json import share_fixed_fields

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
CPP = HERE if (HERE / "device_time_route_harness.cpp.in").is_file() else HERE / "cpp"
ORIGINAL_HASH = "6f91e51f364fba5c19f379414b476443186839525a616e2e6091be789b35a1e4"
OLD = 'FastBee::appendSystemTimeStatus(doc["data"].as<JsonObject>());'
NEW = 'FastBee::appendSystemTimeStatus(doc["data"].to<JsonObject>());'


def definition(source, name):
    pattern = re.compile(r'(?:inline\s+)?void\s+' + re.escape(name) + r'\s*\([^;{]*\)\s*\{')
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise AssertionError("Expected one definition: " + name)
    start = matches[0].start()
    opening = matches[0].end() - 1
    depth = 0
    tokens = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')
    for token in tokens.finditer(source, opening):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[start:token.end()]
    raise AssertionError("Unterminated definition: " + name)


def time_section(source):
    start, end = "    // Device time\n", "    // Device info\n"
    if source.count(start) != 1 or source.count(end) != 1:
        raise AssertionError("Time-route section boundaries changed")
    return source[source.index(start):source.index(end)].rstrip() + "\n"


def route_class(name, section):
    if section.count("JsonDocument doc;") != 3 or section.count("millis()") != 3:
        raise AssertionError("Expected three response documents and uptime reads")
    section = section.replace("JsonDocument doc;", "JsonDocument doc(responseAllocator);")
    section = section.replace("millis()", "FIXED_UPTIME")
    return ("class " + name + " {\npublic:\nFakeContext* ctx;\nexplicit " + name +
            "(FakeContext* context) : ctx(context) {}\nvoid setupRoutes(AsyncWebServer* server) {\n" +
            section + "}\n};\n")


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


class DeviceTimeRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("Registered C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered real ArduinoJson required")
        route = ROOT / "src/network/handlers/DeviceRouteHandler.cpp"
        sender = ROOT / "include/network/handlers/HandlerUtils.h"
        helper = ROOT / "src/network/FixedJsonField.cpp"
        status = ROOT / "include/network/SystemTimeStatus.h"
        oracle = CPP / "device_time_routes_before.inc"
        harness = CPP / "device_time_route_harness.cpp.in"
        prelude = ROOT / "test/unit/cpp/fixed_json_document_prelude.h"
        roots = (CPP, ROOT / "test/unit/cpp", ROOT / "include", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), SUPPORT / "test/unit/test_device_batch_fixed_json.py",
                 route, sender, helper, status, oracle, harness, prelude)
        cls.inputs = sorted(local_inputs(seeds, roots) | set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.addClassCleanup(cls.verify_inputs)
        original = oracle.read_text(encoding="utf-8")
        # Text-normalized hash tolerates Git CRLF checkout, not oracle edits.
        if hashlib.sha256(original.encode("utf-8")).hexdigest() != ORIGINAL_HASH:
            raise AssertionError("Frozen old time-route negative changed")
        if original.count(OLD) != 3 or NEW in original:
            raise AssertionError("Expected exactly three original unbound-object sites")
        current = time_section(route.read_text(encoding="utf-8"))
        baseline = original.replace(OLD, NEW)
        if current != share_fixed_fields(baseline, 14):
            raise AssertionError("Time route changed beyond the three creation fixes and 14 fixed-key writes")
        program = harness.read_text(encoding="utf-8")
        if program.count("@REAL_SENDER@") != 1 or program.count("@ROUTE_CLASSES@") != 1:
            raise AssertionError("Harness expansion markers changed")
        program = program.replace("@REAL_SENDER@", definition(sender.read_text(encoding="utf-8"), "sendJsonStream"))
        program = program.replace("@ROUTE_CLASSES@", route_class("LegacyRoutes", original) +
                                  route_class("BaselineRoutes", baseline) + route_class("CurrentRoutes", current))
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-device-time-")
        cls.addClassCleanup(cls.temporary.cleanup)
        folder = Path(cls.temporary.name)
        generated = folder / "device-time.cpp"
        generated.write_text(program, encoding="utf-8")
        cls.binary = folder / ("device-time.exe" if os.name == "nt" else "device-time")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        built = subprocess.run([
            compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror", "-DUNIT_TEST",
            "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
            "-DARDUINOJSON_ENABLE_STD_STRING=1", *["-I" + str(root) for root in roots],
            "-include", str(prelude), str(generated), str(helper), "-o", str(cls.binary)
        ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=120,
           env=cls.environment)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def verify_inputs(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("Device-time route regression inputs changed while running")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode + " cases=", result.stdout)


for _mode in ("get", "offline", "online", "legacy-unbound", "auth-and-null-network", "allocation-faults", "sharing-differential"):
    setattr(DeviceTimeRouteTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
