"""Full route and import differential; real JSON, shared writers and frozen methods.

No firmware build, networking, configuration device access or physical output.
Unchanged SDK/manager/HTTP delegates are explicit observation fixtures.
"""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from test_periph_exec_fixed_json import definition, local_includes, shared_route_draft

HERE = Path(__file__).resolve().parent
ROOT = Path(os.environ["FASTBEE_TEST_ROOT"]) if os.environ.get("FASTBEE_TEST_ROOT") else next(
    parent for parent in HERE.parents if (parent / "platformio.ini").is_file())
SUPPORT = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
SIGNATURES = ("void writeRuleCandidate(", "String PeriphExecRouteHandler::serializeRuleFull(",
              "void PeriphExecRouteHandler::handlePreflightRuleJson(",
              "void PeriphExecRouteHandler::handleImportChunk(",
              "void PeriphExecRouteHandler::handleRunOnce(")
ORACLE_SHA256 = "8a5cf9094517c17815dcc4bc75bb334675b931f3a4276a829feb36504191ed2e"


def shared_writers(source, old):
    helpers = []
    for kind, type_name, variable, first, last in (
            ("Trigger", "ExecTrigger", "trigger", "triggerType", "pollInterPollDelay"),
            ("Action", "ExecAction", "action", "targetPeriphId", "execMode")):
        pattern = (r'        FastBee::setFixedJsonField\(item, "' + first +
                   r'"[\s\S]*?        FastBee::setFixedJsonField\(item, "' + last + r'"[^\n]+;')
        matches = re.findall(pattern, old[0])
        if len(matches) != 1:
            raise AssertionError("Frozen common rule writer block changed: " + kind)
        signature = "__attribute__((noinline)) void writeRule" + kind + "Fields("
        expected = (signature + "JsonObject item, const " + type_name + "& " + variable + ") {\n" +
                    re.sub(r"^    ", "", matches[0], flags=re.M) + "\n}")
        actual = definition(source, signature)
        if actual != expected:
            raise AssertionError("Shared writer changed field order/expression/type: " + kind)
        helpers.append(actual)
    return "\n\n".join(helpers)


def route_program(source, old, current, shared, harness, ordinary):
    support = "\n".join(line for line in source.splitlines() if line.startswith("constexpr ") and
                        "PERIPH_EXEC_" in line)
    support += "\n\n" + "\n\n".join(definition(source, signature) for signature in (
        "uint32_t intervalSecondsFromInput(", "static int jsonInt(", "bool parseStrictLong(",
        "bool parseClockTime(", "bool actionNeedsPeripheral(", "bool actionTargetMatches(",
        "const char* asyncExecStatusName(", "bool asyncExecStatusIsTerminal(", "int runOnceRejectionStatus("))
    support += "\n\n" + definition(source, "void PeriphExecRouteHandler::parseRuleFromJson(").replace(
        "PeriphExecRouteHandler::", "HandlerBase::", 1)
    control = "\n\n".join(definition(ordinary, signature) for signature in (
        "const char* sourceName(", "const char* phaseName(", "void writeOrdinaryControlJson("))
    copies = []
    for methods, suffix in ((old, "Oracle"), (current, "Current")):
        for index in (0, 2, 4):
            method = methods[index].replace("PeriphExecRouteHandler::", "PeriphExecRouteHandler" + suffix + "::", 1)
            method = method.replace("writeRuleCandidate(", "writeRuleCandidate" + suffix + "(")
            copies.append(method)
    replacements = {"@SUPPORT_METHODS@": support, "@SHARED_WRITERS@": shared,
                    "@ORDINARY_WRITER@": control, "@ROUTE_METHODS@": "\n\n".join(copies)}
    for marker, value in replacements.items():
        if harness.count(marker) != 1:
            raise AssertionError("Expected one route fixture marker: " + marker)
        harness = harness.replace(marker, value, 1)
    return harness


def import_program(fixture, tail, original, allocator, visitor):
    # Reuse the upgraded original import filesystem/auth/range/commit fixture.
    fixture = fixture[:fixture.index("static void gates()")]
    marker = "r->status=200;r->result=result;"
    if fixture.count(marker) != 1:
        raise AssertionError("Import send-success observation seam changed")
    fixture = fixture.replace(marker, "importDocuments.push_back(importCapture(result));" + marker, 1)
    prologue = """#include <ArduinoJson.h>
#include <ArduinoJson/Variant/JsonVariantVisitor.hpp>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <type_traits>
""" + allocator + "\n" + visitor + """
static DocumentProbeAllocator* importAllocator=nullptr;
static std::vector<std::string> importDocuments;
static std::string importCapture(const JsonDocument& doc){
    std::string bytes;serializeJson(doc,bytes);JsonShape visitor;
    return std::to_string(doc.size())+":"+std::to_string(doc.nesting())+":"+
        std::to_string(doc.overflowed())+":"+ArduinoJson::detail::accept(doc.as<JsonVariantConst>(),visitor)+":"+bytes;
}
"""
    original = original.replace("PeriphExecRouteHandler::", "PeriphExecRouteHandlerOracle::", 1)
    original = original.replace("JsonDocument result;", "JsonDocument result(importAllocator);", 1)
    if tail.count("@ORACLE_IMPORT@") != 1:
        raise AssertionError("Import oracle boundary changed")
    return prologue + fixture + tail.replace("@ORACLE_IMPORT@", original, 1)


class PeriphRouteSharedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        if not Path(compiler).is_file():
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else SUPPORT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered ArduinoJson headers required")
        source_path = ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
        header_path = ROOT / "include/network/handlers/PeriphExecRouteHandler.h"
        ordinary_path = ROOT / "src/network/HttpOutputControl.cpp"
        oracle_path = HERE / "cpp/periph_route_shared_json_before.inc"
        route_harness = HERE / "cpp/periph_route_shared_json_harness.cpp.in"
        import_harness = HERE / "cpp/periph_import_shared_json_harness.cpp.in"
        import_fixture = HERE / "cpp/periph_exec_import_chunk_harness.cpp"
        probe_path = HERE / "cpp/document_json_probe.h"
        old_harness = HERE / "cpp/periph_exec_fixed_json_harness.cpp.in"
        support_path = HERE / "test_periph_exec_fixed_json.py"
        helper = ROOT / "src/network/FixedJsonField.cpp"
        helper_header = ROOT / "include/network/FixedJsonField.h"
        prelude = HERE / "cpp/fixed_json_document_prelude.h"
        mock = SUPPORT / "test/mocks/Arduino.h"
        source = source_path.read_text(encoding="utf-8")
        oracle = oracle_path.read_text(encoding="utf-8")
        if hashlib.sha256(oracle.encode("utf-8")).hexdigest() != ORACLE_SHA256:
            raise AssertionError("Frozen complete route methods changed")
        old = [definition(oracle, signature) for signature in SIGNATURES]
        current = [definition(source, signature) for signature in SIGNATURES]
        if current != [shared_route_draft(method) for method in old]:
            raise AssertionError("Routes changed beyond the reviewed 30 writes/common field extraction")
        shared = shared_writers(source, old)
        allocator = definition(probe_path.read_text(encoding="utf-8"), "class DocumentProbeAllocator") + ";"
        visitor = definition(old_harness.read_text(encoding="utf-8"), "struct Shape").replace(
            "struct Shape", "struct JsonShape", 1).replace("CHECK(false);", "assert(false);") + ";"
        programs = {
            "routes": route_program(source, old, current, shared, route_harness.read_text(encoding="utf-8"),
                                     ordinary_path.read_text(encoding="utf-8")),
            "import": import_program(import_fixture.read_text(encoding="utf-8"),
                                     import_harness.read_text(encoding="utf-8"), old[3], allocator, visitor),
        }
        roots = (HERE / "cpp", ROOT / "include", SUPPORT / "include", SUPPORT / "test/mocks", library)
        seeds = (Path(__file__), source_path, header_path, ordinary_path, oracle_path, route_harness,
                 import_harness, import_fixture, probe_path, old_harness, support_path,
                 helper, helper_header, prelude, mock)
        cls.inputs = sorted(local_includes(seeds, roots) |
            set(library.rglob("*.h")) | set(library.rglob("*.hpp")))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-route-sharing-")
        cls.addClassCleanup(cls.temporary.cleanup)
        temporary = Path(cls.temporary.name)
        header = header_path.read_text(encoding="utf-8")
        fields = header[header.index("    String importSession;"):header.index("    void handleImportChunk(")]
        (temporary / "production_import_fields.inc").write_text(fields, encoding="utf-8")
        (temporary / "production_import_chunk.inc").write_text(
            current[3].replace("JsonDocument result;", "JsonDocument result(importAllocator);", 1), encoding="utf-8")
        arduino = mock.read_text(encoding="utf-8")
        marker = "void clear() { _value.clear(); }"
        if arduino.count(marker) != 1:
            raise AssertionError("Arduino String writer fixture changed")
        arduino = arduino.replace(marker, marker +
            '\n    bool concat(const char* p) { _value += p ? p : ""; return true; }', 1)
        (temporary / "Arduino.h").write_text(arduino, encoding="utf-8")
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        cls.binaries = {}
        for label, program in programs.items():
            cpp = temporary / (label + ".cpp")
            cpp.write_text(program, encoding="utf-8")
            binary = temporary / (label + (".exe" if os.name == "nt" else ""))
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-DUNIT_TEST", "-DCONFIG_IDF_TARGET_ESP32C6=1", "-DFASTBEE_ENABLE_PERIPH_EXEC=1",
                "-DFASTBEE_TEST_NEEDS_STRLCPY=1", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=" + ("1" if label == "routes" else "0"),
                "-I" + str(temporary), *["-I" + str(path) for path in roots]]
            if label == "routes":
                command += ["-include", str(prelude)]
            command += [str(cpp), str(helper), "-o", str(binary)]
            built = subprocess.run(command, text=True, capture_output=True, encoding="utf-8",
                                   errors="replace", timeout=120, env=cls.environment)
            if built.returncode:
                raise AssertionError(built.stdout + built.stderr)
            cls.binaries[label] = binary

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        if after != cls.hashes:
            raise AssertionError("Route sharing verification inputs changed during the run")

    def check(self, mode):
        binary = self.binaries["import" if mode.startswith("import") else "routes"]
        result = subprocess.run([str(binary), mode], text=True, capture_output=True,
                                encoding="utf-8", errors="replace", timeout=90, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("preflight", "preflight-oom", "runonce", "runonce-oom", "import", "import-oom"):
    setattr(PeriphRouteSharedJsonTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
