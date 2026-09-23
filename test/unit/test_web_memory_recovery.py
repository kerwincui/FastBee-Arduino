"""Execute the production WebGuard state machine with a controlled heap/clock."""
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = pathlib.Path(__file__).resolve().parents[2]


class WebMemoryRecoveryTests(unittest.TestCase):
    def test_production_guard_recovery_and_unmodified_admission(self):
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        source = (ROOT / "src/network/WebMemoryGuard.cpp").read_text(encoding="utf-8")
        candidates = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        self.assertTrue(candidates, "ArduinoJson dependency required")
        signature = "void WebMemoryGuardMiddleware::maintain()"
        with tempfile.TemporaryDirectory(prefix="fastbee-web-recovery-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "core").mkdir()
            (temporary / "core/FeatureFlags.h").write_text("#pragma once\n", encoding="utf-8")
            (temporary / "Arduino.h").write_text('''#pragma once
#include <string>
class String {
public:
 size_t capacity=10000;
 std::string bytes;
 String& operator=(const char* value) {bytes=value?value:"";return *this;}
 bool concat(const char* value) {
   if(bytes.size()+std::char_traits<char>::length(value)>capacity) return false;
   bytes+=value;return true;
 }
 size_t length() const {return bytes.size();}
};
''', encoding="utf-8")
            (temporary / "esp_heap_caps.h").write_text('''#pragma once
#include <cstdint>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
extern uint32_t testFree, testLargest;
inline uint32_t heap_caps_get_free_size(int) {return testFree;}
inline uint32_t heap_caps_get_largest_free_block(int) {return testLargest;}
''', encoding="utf-8")
            (temporary / "ESPAsyncWebServer.h").write_text('''#pragma once
#include <cstdint>
class AsyncWebServerRequest {};
using ArMiddlewareNext = void(*)();
class AsyncMiddleware {public: virtual ~AsyncMiddleware()=default;
virtual void run(AsyncWebServerRequest*, ArMiddlewareNext)=0;};
''', encoding="utf-8")
            (temporary / "production_maintain.inc").write_text(
                signature + " {" + braced_block(source, signature) + "}\n", encoding="utf-8")
            route = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(encoding="utf-8")
            body = braced_block(route, "void SystemRouteHandler::handleSystemStatus")
            predicate = re.search(r"if \((written != expectedSize[^\n]+)\) \{", body).group(1)
            (temporary / "production_status_rejected.inc").write_text(
                "return " + predicate + ";\n", encoding="utf-8")
            executable = temporary / "guard.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-DESP_PLATFORM", "-DFASTBEE_WEB_MEM_GUARD=1", f"-I{temporary}",
                       f"-I{ROOT / 'include'}", f"-I{candidates[0].parent}",
                       str(ROOT / "test/unit/cpp/web_memory_recovery_harness.cpp"),
                       "-o", str(executable)]
            build = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, env=environment)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_status_builds_one_document_and_releases_before_response(self):
        source = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(encoding="utf-8")
        body = braced_block(source, "void SystemRouteHandler::handleSystemStatus")
        self.assertEqual(1, body.count("FastBee::makeJsonDocument("))
        self.assertIn('JsonObject doc = wrapper["data"].to<JsonObject>()', body)
        self.assertNotIn('wrapper["data"] = doc', body)
        serialization = body.index("serializeJson(wrapper, _statusCache.json)")
        release = body.index("wrapper.clear()", serialization)
        response = body.index("request->beginResponse", serialization)
        self.assertLess(release, response)
        self.assertIn("wrapper.overflowed()", body)
        self.assertIn("written != expectedSize", body)

    def test_server_recovery_uses_same_streaming_headroom(self):
        source = (ROOT / "src/network/WebConfigManager.cpp").read_text(encoding="utf-8")
        start = source.index("const bool severeWebPressure =")
        predicate = source[start:source.index(";", start)]
        self.assertIn("isSevereWebPressure8(", predicate)
        self.assertIn("!FastBee::MemoryBudget::hasWebStreamingHeadroom(", predicate)


if __name__ == "__main__":
    unittest.main()
