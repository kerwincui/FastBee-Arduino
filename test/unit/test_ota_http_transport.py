"""Exercise production URL OTA code with observable HTTP/TLS clients."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class OtaHttpTransportTests(unittest.TestCase):
    def test_memory_gate_requires_internal_byte_addressable_ram_without_health_monitor(self):
        source = (ROOT / "src/network/OTAManager.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/network/OTAManager.h").read_text(encoding="utf-8")
        threshold = re.search(r"static const uint32_t OTA_MIN_FREE_DRAM\s*=\s*(\d+);", header)
        self.assertIsNotNone(threshold, "Production OTA threshold must be available")
        self.assertEqual(20480, int(threshold.group(1)), "Keep the existing 20 KiB admission threshold")
        method = definition(source, "bool OTAManager::checkMemoryGate(")
        cpp = r'''
#include <cassert>
#include "Arduino.h"
#include "core/MemoryBudget.h"
#if FASTBEE_ENABLE_HEALTH_MONITOR
enum class MemoryGuardLevel { NORMAL, WARN, SEVERE, CRITICAL };
struct HealthMonitor {
 MemoryGuardLevel level=MemoryGuardLevel::NORMAL;
 MemoryGuardLevel getMemoryGuardLevel() { return level; }
} health;
bool frameworkAvailable=true,monitorAvailable=true;
struct FastBeeFramework {
 static FastBeeFramework* getInstance() {
  static FastBeeFramework instance;return frameworkAvailable?&instance:nullptr;
 }
 HealthMonitor* getHealthMonitor() { return monitorAvailable?&health:nullptr; }
};
#endif
struct OTAManager {
 static const uint32_t OTA_MIN_FREE_DRAM=THRESHOLD;
 bool checkMemoryGate(String* reason=nullptr);
};
PRODUCTION
int main() {
 OTAManager ota;String reason;
 // Both PSRAM and internal 32-bit-only heaps remain large. Neither may
 // satisfy the free byte-addressable internal RAM requirement.
 for (const size_t value : {size_t(0),size_t(4096),size_t(20479)}) {
  internalFree=value;reason="old";
  assert(!ota.checkMemoryGate(&reason));
  assert(reason=="Insufficient memory for OTA update (DRAM="+String(value)+"B)");
  assert(!ota.checkMemoryGate());
 }
 internalFree=20480;reason="untouched";
 assert(ota.checkMemoryGate(&reason));assert(reason=="untouched");
 assert(ota.checkMemoryGate());
#if FASTBEE_ENABLE_HEALTH_MONITOR
 health.level=MemoryGuardLevel::WARN;assert(ota.checkMemoryGate());
 for (const auto level : {MemoryGuardLevel::SEVERE,MemoryGuardLevel::CRITICAL}) {
  health.level=level;assert(!ota.checkMemoryGate(&reason));
  assert(reason=="Memory pressure too high for OTA, retry later");
  assert(!ota.checkMemoryGate());
 }
 // Preserve guard precedence when both guard and raw pool reject.
 internalFree=0;assert(!ota.checkMemoryGate(&reason));
 assert(reason=="Memory pressure too high for OTA, retry later");
 health.level=MemoryGuardLevel::NORMAL;
 monitorAvailable=false;assert(!ota.checkMemoryGate(&reason));
 assert(reason=="Insufficient memory for OTA update (DRAM=0B)");
 frameworkAvailable=false;assert(!ota.checkMemoryGate());
 internalFree=20480;assert(ota.checkMemoryGate());
#endif
}
'''.replace('THRESHOLD', threshold.group(1)).replace('PRODUCTION', method)
        compiler = shutil.which("g++") or shutil.which("clang++") or "D:/msys64/ucrt64/bin/g++.exe"
        self.assertTrue(Path(compiler).exists(), "C++17 compiler required")
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with tempfile.TemporaryDirectory(prefix="fastbee-ota-memory-") as directory:
            temporary = Path(directory)
            (temporary / "esp_heap_caps.h").write_text(
                (ROOT / "test/unit/cpp/memory_gate_heap_mock.h").read_text(encoding="utf-8"), encoding="utf-8")
            path = temporary / "memory.cpp"
            path.write_text(cpp, encoding="utf-8")
            for enabled in (0, 1):
                with self.subTest(health_monitor=enabled):
                    executable = temporary / (f"memory-{enabled}.exe" if os.name == "nt" else f"memory-{enabled}")
                    build = subprocess.run(
                        [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DESP_PLATFORM=1",
                         f"-DFASTBEE_ENABLE_HEALTH_MONITOR={enabled}", f"-I{temporary}",
                         f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}", str(path), "-o", str(executable)],
                        capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=environment)
                    self.assertEqual(0, build.returncode, build.stdout + build.stderr)
                    run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, env=environment)
                    self.assertEqual(0, run.returncode, run.stdout + run.stderr)

    def test_production_download_uses_one_client_and_preserves_failure_gates(self):
        source = (ROOT / "src/network/OTAManager.cpp").read_text(encoding="utf-8")
        start = source.index("bool OTAManager::startOTA(")
        end = source.index("bool OTAManager::startOTAAsync(", start)
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        self.assertTrue(Path(compiler).exists(), "C++ compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-ota-http-") as directory:
            temporary = Path(directory)
            (temporary / "production_ota_download.inc").write_text(source[start:end], encoding="utf-8")
            executable = temporary / "ota-http.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                 "-DUSE_NATIVE_STDLIB=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                 "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                 f"-I{temporary}", f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                 f"-I{ROOT / '.pio/libdeps/native/ArduinoJson/src'}",
                 str(ROOT / "test/unit/cpp/ota_http_transport_harness.cpp"), "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15, env=environment)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)

    def test_installed_http_client_rejects_cross_scheme_redirects(self):
        # Production uses strict redirects on the supplied client. Verify the
        # installed dependency cannot silently downgrade HTTPS to plain HTTP.
        framework = Path.home() / ".platformio/packages/framework-arduinoespressif32"
        source = (framework / "libraries/HTTPClient/src/HTTPClient.cpp").read_text(encoding="utf-8")
        body = source[source.index("bool HTTPClient::setURL("):]
        self.assertIn('if (!url.startsWith(_protocol +', body)
        guard = body[:body.index("// check if the port is specified")]
        self.assertIn("return false;", guard)


if __name__ == "__main__":
    unittest.main()
