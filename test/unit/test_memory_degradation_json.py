"""Execute the production degradation helper against real ArduinoJson objects."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class MemoryDegradationJsonTests(unittest.TestCase):
    def test_degradation_merges_without_erasing_memory_metrics(self):
        header = (ROOT / 'include/network/handlers/HandlerUtils.h').read_text(encoding='utf-8')
        helper = braced_block(header, 'inline void addMemoryDegradationStatus(JsonObject parent)')
        overload = braced_block(header, 'inline void addMemoryDegradationStatus(JsonDocument& parent)')
        names = braced_block(header, 'inline const char* memoryGuardLevelName(MemoryGuardLevel level)')
        program = r'''
#include <ArduinoJson.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "failed: %s\n", #c); std::exit(86); } } while (false)
enum class MemoryGuardLevel { NORMAL, WARN, SEVERE, CRITICAL };
struct HealthMonitor {
 MemoryGuardLevel level = MemoryGuardLevel::NORMAL;
 bool paused = false;
 MemoryGuardLevel getMemoryGuardLevel() const { return level; }
 bool isMemoryDegraded() const { return paused; }
 bool isPeriphExecPausedForMemory() const { return paused; }
 bool arePeriphExecWorkersSuspendedForMemory() const { return paused; }
 bool isPeriphExecWorkerSuspensionPending() const { return false; }
 bool isModbusStoppedForMemory() const { return false; }
 bool isMqttRestorePendingForMemory() const { return false; }
 bool areOptionalPeripheralsSuspendedForMemory() const { return false; }
 unsigned getMemorySuspendedPeripheralCount() const { return 0; }
 void fillDegradationJson(JsonObject status) const { status["active"] = paused; }
} monitor;
bool haveMonitor = true;
struct FastBeeFramework {
 static FastBeeFramework* getInstance() { static FastBeeFramework f; return &f; }
 HealthMonitor* getHealthMonitor() { return haveMonitor ? &monitor : nullptr; }
};
namespace HandlerUtils {
inline const char* memoryGuardLevelName(MemoryGuardLevel level) { NAMES }
inline void addMemoryDegradationStatus(JsonObject parent) { HELPER }
inline void addMemoryDegradationStatus(JsonDocument& parent) { OVERLOAD }
}
int main() {
 JsonDocument doc;
 auto data = doc["data"].to<JsonObject>();
 auto memory = data["memory"].to<JsonObject>();
 memory["heapTotal"] = 327680;
 memory["heapFree"] = 21968;
 memory["heapUsed"] = 305712;
 memory["heapMinFree"] = 12000;
 memory["psramTotal"] = 8388608;
 memory["psramFree"] = 8192000;
 memory["free"] = 8213968;
 memory["total"] = 8716288;
 memory["staticPools"]["used"] = 7;
 HandlerUtils::addMemoryDegradationStatus(data);
 CHECK(data["memory"]["heapFree"].as<unsigned>() == 21968);
 CHECK(data["memory"]["heapTotal"].as<unsigned>() == 327680);
 CHECK(data["memory"]["heapUsed"].as<unsigned>() == 305712);
 CHECK(data["memory"]["heapMinFree"].as<unsigned>() == 12000);
 CHECK(data["memory"]["psramTotal"].as<unsigned>() == 8388608);
 CHECK(data["memory"]["psramFree"].as<unsigned>() == 8192000);
 CHECK(data["memory"]["free"].as<unsigned>() == 8213968);
 CHECK(data["memory"]["total"].as<unsigned>() == 8716288);
 CHECK(data["memory"]["staticPools"]["used"].as<unsigned>() == 7);
 CHECK(std::string(data["memory"]["guardLevel"].as<const char*>()) == "NORMAL");
 monitor.level = MemoryGuardLevel::SEVERE; monitor.paused = true;
 HandlerUtils::addMemoryDegradationStatus(data);
 CHECK(data["memory"]["heapFree"].as<unsigned>() == 21968);
 CHECK(std::string(data["memory"]["guardLevel"].as<const char*>()) == "SEVERE");
 CHECK(data["memory"]["periphExecPaused"].as<bool>());
 CHECK(data["degraded"].as<bool>() && data["memoryDegradation"]["active"].as<bool>());
 JsonDocument empty;
 empty.to<JsonObject>();
 HandlerUtils::addMemoryDegradationStatus(empty);
 CHECK(empty["memory"].is<JsonObject>());
 CHECK(empty["memory"]["periphExecWorkerSuspended"].as<bool>());
 CHECK(empty["degraded"].as<bool>());
 haveMonitor = false;
 std::string before, after;
 serializeJson(doc, before);
 HandlerUtils::addMemoryDegradationStatus(data);
 serializeJson(doc, after);
 CHECK(before == after);
}
'''.replace('NAMES', names).replace('HELPER', helper).replace('OVERLOAD', overload)
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        includes = ROOT / '.pio/libdeps/native/ArduinoJson/src'
        self.assertTrue((includes / 'ArduinoJson.h').is_file(), 'Real ArduinoJson headers required')
        with tempfile.TemporaryDirectory(prefix='fastbee-memory-json-') as directory:
            source = Path(directory) / 'test.cpp'
            executable = Path(directory) / ('test.exe' if os.name == 'nt' else 'test')
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')

            def run(code):
                source.write_text(code, encoding='utf-8')
                built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(includes), str(source), '-o', str(executable)],
                    capture_output=True, text=True, timeout=40, env=env)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                return subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, env=env)

            result = run(program)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            old = program.replace('JsonObject memory = parent["memory"].as<JsonObject>();',
                                  'JsonObject memory = parent["memory"].to<JsonObject>();')
            self.assertNotEqual(old, program)
            result = run(old)
            self.assertEqual(result.returncode, 86, result.stdout + result.stderr)
            self.assertIn('heapFree', result.stderr)


if __name__ == '__main__':
    unittest.main()
