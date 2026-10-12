"""Full recent-results response with the actual AJ7 Arduino String adapter."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_fixed_json import definition, local_includes

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


class PeriphExecRecentResultsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("C++17 compiler required")
        library = Path(os.environ["FASTBEE_TEST_ARDUINOJSON_INCLUDE"]) if os.environ.get(
            "FASTBEE_TEST_ARDUINOJSON_INCLUDE") else ROOT / ".pio/libdeps/native/ArduinoJson/src"
        if not (library / "ArduinoJson.h").is_file():
            raise RuntimeError("Registered ArduinoJson headers required")
        route_path = ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
        control_path = ROOT / "src/network/HttpOutputControl.cpp"
        types_path = ROOT / "include/core/AsyncExecTypes.h"
        oracle_path = HERE / "cpp/recent_results_before.inc"
        harness_path = HERE / "cpp/recent_results_harness.cpp.in"
        fault_path = HERE / "cpp/recent_results_string_fault.inc"
        response_mock_path = HERE / "cpp/recent_results_response_mock.h"
        mock_path = ROOT / "test/mocks/Arduino.h"
        helper_path = ROOT / "src/utils/SharedJsonSerializer.cpp"
        support = Path(os.environ.get("FASTBEE_TEST_SUPPORT_ROOT", ROOT))
        server_source = support / ".pio/libdeps/esp32c3-F4R0/ESPAsyncWebServer/src/WebResponses.cpp"
        if not server_source.is_file():
            server_source = next((support / ".pio/libdeps").glob("*/ESPAsyncWebServer/src/WebResponses.cpp"), None)
        if server_source is None:
            raise RuntimeError("Registered ESPAsyncWebServer source required for body-copy regression")
        basic_constructor = definition(server_source.read_text(encoding="utf-8"),
                                       "AsyncBasicResponse::AsyncBasicResponse(")
        if "_content = content;" not in basic_constructor:
            raise AssertionError("Installed BasicResponse ownership seam changed")
        source = route_path.read_text(encoding="utf-8")
        control = control_path.read_text(encoding="utf-8")
        types = types_path.read_text(encoding="utf-8")
        signatures = ("enum class AsyncExecStatus : uint8_t",
                      "enum class AsyncExecError : uint8_t",
                      "struct AsyncExecActionTrace", "struct AsyncExecResult")
        actual_types = "\n\n".join(definition(types, marker) + ";" for marker in signatures)
        limit = re.search(r"static constexpr size_t ASYNC_EXEC_ACTION_TRACE_LIMIT = [^;]+;", types)
        if not limit:
            raise AssertionError("Actual trace limit not found")
        actual_types = limit.group() + "\n" + actual_types
        actual_control = "\n\n".join(definition(control, signature) for signature in (
            "const char* sourceName(", "const char* phaseName(", "void writeOrdinaryControlJson("))
        dependencies = "\n\n".join(definition(source, signature) for signature in (
            "String escapePeriphExecJsonString(", "const char* asyncExecStatusName(",
            "const char* asyncExecLegacyStatusName(", "const char* asyncExecErrorName(",
            "bool asyncExecStatusIsTerminal("))
        signature = "void PeriphExecRouteHandler::handleGetRecentResults("
        actual = definition(source, signature)
        before = definition(oracle_path.read_text(encoding="utf-8"), signature)
        broken_call = "serializeJson(control, items);"
        if before.count(broken_call) != 1:
            raise AssertionError("Frozen regression must retain the original String writer call")
        reference = before.replace(broken_call, "referenceControlAppend(control, items);")
        routes = []
        for name, body in (("ActualRoute", actual), ("BeforeRoute", before), ("ReferenceRoute", reference)):
            if body.count("JsonDocument control;") != 1:
                raise AssertionError("Expected one default control-document allocation seam")
            # Allocation seam only; all response logic and the real serializer remain intact.
            body = body.replace("JsonDocument control;", "JsonDocument control(&routeAllocator);")
            routes.append(body.replace("PeriphExecRouteHandler::", name + "::", 1))
        program = harness_path.read_text(encoding="utf-8")
        for marker, content in (("@PRODUCTION_TYPES@", actual_types),
                                ("@CONTROL_SERIALIZER@", actual_control),
                                ("@ROUTE_DEPENDENCIES@", dependencies),
                                ("@PRODUCTION_ROUTES@", "\n\n".join(routes))):
            if program.count(marker) != 1:
                raise AssertionError("Harness marker drift: " + marker)
            program = program.replace(marker, content)
        arduino = mock_path.read_text(encoding="utf-8")
        replacements = {
            "class String {": fault_path.read_text(encoding="utf-8") + "\nclass String {",
            "    String() = default;": """    String() = default;
    String(const String& other) : _value(other._value) {}
    String(String&& other) noexcept : _value(std::move(other._value)) { other._value.clear(); }
    String& operator=(const String& other) { _value = other._value; return *this; }
    String& operator=(String&& other) noexcept {
        if (recentMoveFailAt >= 0 && recentMoveCalls++ == static_cast<size_t>(recentMoveFailAt)) {
            _value.clear(); return *this;
        }
        _value = std::move(other._value); other._value.clear(); return *this;
    }
    static void* operator new[](size_t size, const std::nothrow_t&) noexcept {
        if (recentArrayFailure) return nullptr;
        void* memory = ::operator new[](size, std::nothrow);
        if (memory) ++recentLiveArrays;
        return memory;
    }
    static void operator delete[](void* memory) noexcept {
        if (memory) { --recentLiveArrays; }
        ::operator delete[](memory);
    }
    static void operator delete[](void* memory, const std::nothrow_t&) noexcept { operator delete[](memory); }
""",
            'String(const char* value) : _value(value ? value : "") {}':
                'String(const char* value) : _value(recentFooterFailure && value && std::strcmp(value, "]}") == 0 ? "" : (value ? value : "")) {}',
            '        _value = value ? value : "";': """        if (recentBodyCopyActive) {
            ++recentBodyCopies;
            if (recentBodyCopyFailure) { ++recentBodyCopyFailures; _value.clear(); return *this; }
        }
        _value = value ? value : "";""",
            "    std::string _value;": "    std::string _value;\n    RecentStringProbe recentProbe_{&_value};",
            "bool reserve(size_t size) { _value.reserve(size); return true; }":
                "bool reserve(size_t size) { if (recentTextFault.fail()) return false; _value.reserve(size); return true; }",
            "bool concat(const char* value, unsigned int length) { if (!value && length) return false; if (length) _value.append(value, length); return true; }":
                """bool concat(const char* value, unsigned int length) {
                    if (!value && length) return false;
                    if (recentTextFault.fail()) {
                        if (recentTextFault.partial && length > 1) _value.append(value, length / 2);
                        return recentTextFault.lie;
                    }
                    if (length) _value.append(value, length);
                    return true;
                }
                bool concat(const char* value) {
                    return concat(value, static_cast<unsigned int>(value ? std::strlen(value) : 0));
                }"""
        }
        for old, new in replacements.items():
            if arduino.count(old) != 1:
                raise AssertionError("Arduino storage seam changed: " + old[:45])
            arduino = arduino.replace(old, new, 1)
        cls.temp_directory = tempfile.TemporaryDirectory(prefix="fastbee-recent-results-")
        cls.addClassCleanup(cls.temp_directory.cleanup)
        temp = Path(cls.temp_directory.name)
        (temp / "Arduino.h").write_text(arduino, encoding="utf-8")
        shutil.copyfile(response_mock_path, temp / "ESPAsyncWebServer.h")
        (temp / "recent_basic_constructor.inc").write_text(basic_constructor + "\n", encoding="utf-8")
        cpp = temp / "recent_results.cpp"
        cpp.write_text(program, encoding="utf-8")
        cls.binary = temp / ("recent_results.exe" if os.name == "nt" else "recent_results")
        roots = (ROOT / "include", ROOT / "test/mocks", library)
        seeds = (ROOT / "include/core/PeripheralExecution.h", ROOT / "include/core/NativeOutputControl.h",
                 ROOT / "include/utils/SharedJsonSerializer.h", ROOT / "include/network/JsonFragmentsResponse.h",
                 mock_path, helper_path)
        cls.inputs = sorted(local_includes(seeds, roots) |
                            {Path(__file__), route_path, control_path, types_path,
                             oracle_path, harness_path, fault_path, response_mock_path, server_source})
        cls.inputs += sorted(library.rglob("*.h")) + sorted(library.rglob("*.hpp"))
        cls.hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}
        cls.environment = dict(os.environ)
        cls.environment["PATH"] = str(Path(compiler).parent) + os.pathsep + cls.environment.get("PATH", "")
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-DUNIT_TEST", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                   "-I" + str(temp), *["-I" + str(root) for root in roots],
                   str(cpp), str(helper_path), "-o", str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", env=cls.environment, timeout=120)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs}:
            raise AssertionError("Recent-results verification inputs changed during the run")

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", env=self.environment, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS " + mode, result.stdout)


for _mode in ("adapter", "before-repro", "values", "string-faults", "json-faults", "auth",
              "send-copy-failure", "owned-lifetime", "response-faults", "owned-fragment-contract"):
    setattr(PeriphExecRecentResultsTests, "test_" + _mode.replace("-", "_"),
            lambda self, mode=_mode: self.check(mode))

if __name__ == "__main__":
    unittest.main()
