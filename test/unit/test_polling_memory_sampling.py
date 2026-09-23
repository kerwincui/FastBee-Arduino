"""Exercise real checkTimers gates with internal and external heaps separated."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class PollingMemorySamplingTests(unittest.TestCase):
    def test_psram_cannot_satisfy_internal_polling_reserve(self):
        scheduler = (ROOT / 'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        health = (ROOT / 'include/systems/HealthMonitor.h').read_text(encoding='utf-8')
        namespace = definition(scheduler, 'namespace {')
        method = definition(scheduler, 'void PeriphExecScheduler::checkTimers()')
        # Execute the production admission and heartbeat prefix, stopping only
        # before unrelated timer iteration/hardware work becomes possible.
        prefix = method[:method.index('    // 等级变化时更新检查周期并输出日志')]
        prefix += '    ++admitted;\n}\n'
        program = r'''
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/ChipConfig.h"
#include "core/MemoryBudget.h"
#include "utils/HeapFragmentation.h"
ENUM;
NAMESPACE
uint32_t internalFree=6000, internalLargest=3000, externalFree=8*1024*1024;
uint32_t heap_caps_get_free_size(uint32_t caps) {
 return (caps & MALLOC_CAP_INTERNAL) ? internalFree : internalFree+externalFree;
}
uint32_t heap_caps_get_largest_free_block(uint32_t caps) {
 return (caps & MALLOC_CAP_INTERNAL) ? internalLargest : (externalFree ? externalFree : internalLargest);
}
uint32_t nowMs=200001;
unsigned long millis(){return nowMs;}
struct HealthMonitor {
 MemoryGuardLevel getMemoryGuardLevel(){return MemoryGuardLevel::NORMAL;}
} health;
struct FastBeeFramework {
 static FastBeeFramework* getInstance(){static FastBeeFramework f;return &f;}
 HealthMonitor* getHealthMonitor(){return &health;}
};
struct { template<class... T> void warningf(const char*,T...){} template<class... T> void infof(const char*,T...){}
 void warning(const char*){} void info(const char*){} } LOGGER;
struct { void println(const char*){} } Serial;
struct PeriphExecScheduler {
 bool _memoryPressurePaused=false, _webReserveSuspended=false;
 uint32_t _memoryPressurePausedSince=1, _webReserveSuspendedSince=1;
 MemoryGuardLevel _lastMemGuardLevel=MemoryGuardLevel::NORMAL;
 uint32_t _currentCheckPeriodMs=1000;
 static constexpr uint32_t CHECK_PERIOD_SEVERE_MS=4000;
 void* _manager=this;
 unsigned admitted=0;
 void checkTimers();
};
METHOD
#define CHECK(c) do { if (!(c)) {std::fprintf(stderr,"failed: %s\n",#c);std::exit(86);} } while(false)
int main(int argc,char** argv){
 CHECK(argc==2);
 PeriphExecScheduler s;
 if(std::strcmp(argv[1],"paused")==0){
   s._memoryPressurePaused=true;s._webReserveSuspended=true;
   s.checkTimers();
   CHECK(s.admitted==0 && s._memoryPressurePausedSince==1);
 } else if(std::strcmp(argv[1],"free")==0){
   internalLargest=32768; // Isolate the free-byte sample from block admission.
   s.checkTimers();CHECK(s.admitted==0 && s._webReserveSuspended);
 } else if(std::strcmp(argv[1],"block")==0){
   internalFree=65536;
   s.checkTimers();CHECK(s.admitted==0 && s._webReserveSuspended);
 } else {
   for(uint32_t outside : {0U,8U*1024U*1024U}) {
     externalFree=outside;internalFree=65536;internalLargest=32768;
     s=PeriphExecScheduler{};s._webReserveSuspended=true;
     s.checkTimers();CHECK(s.admitted==1 && !s._webReserveSuspended);
     internalFree=6000;internalLargest=3000;s=PeriphExecScheduler{};
     s.checkTimers();CHECK(s.admitted==0 && s._webReserveSuspended);
   }
 }
}
'''.replace('ENUM', definition(health, 'enum class MemoryGuardLevel')) \
   .replace('NAMESPACE', namespace).replace('METHOD', prefix)
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-polling-sample-') as directory:
            temporary = Path(directory)
            source = temporary / 'sample.cpp'
            binary = temporary / ('sample.exe' if os.name == 'nt' else 'sample')
            (temporary / 'esp_heap_caps.h').write_text(
                '#include <cstdint>\n#define MALLOC_CAP_INTERNAL 1U\n#define MALLOC_CAP_8BIT 2U\n'
                'uint32_t heap_caps_get_free_size(uint32_t);\n'
                'uint32_t heap_caps_get_largest_free_block(uint32_t);\n', encoding='utf-8')
            env = dict(os.environ)
            env['PATH'] = str(Path(compiler).parent) + os.pathsep + env.get('PATH', '')

            def build(code, chip):
                source.write_text(code, encoding='utf-8')
                compiled = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-include', 'initializer_list', '-DESP_PLATFORM=1', '-D' + chip + '=1',
                    '-I' + str(temporary), '-I' + str(ROOT / 'include'), str(source), '-o', str(binary)],
                    capture_output=True, text=True, timeout=40, env=env)
                self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)

            def run(mode):
                return subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=10, env=env)

            for chip in ['CONFIG_IDF_TARGET_ESP32S3', 'CONFIG_IDF_TARGET_ESP32C3', 'CONFIG_IDF_TARGET_ESP32']:
                build(program, chip)
                for mode in ['paused', 'free', 'block', 'recovery']:
                    result = run(mode)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for mode, current, old in [
                ('paused', 'pausedDram = FastBee::MemoryBudget::dram8Free()', 'pausedDram = heap_caps_get_free_size(MALLOC_CAP_8BIT)'),
                ('free', 'freeHeap = FastBee::MemoryBudget::dram8Free()', 'freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT)'),
                ('block', 'largestBlock = FastBee::MemoryBudget::dram8Largest()', 'largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)')]:
                mutated = program.replace(current, old)
                self.assertNotEqual(mutated, program)
                build(mutated, 'CONFIG_IDF_TARGET_ESP32S3')
                result = run(mode)
                self.assertEqual(result.returncode, 86, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
