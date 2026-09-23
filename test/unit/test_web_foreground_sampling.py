"""Run real foreground request/maintenance methods with a controlled task clock."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class WebForegroundSamplingTests(unittest.TestCase):
    def test_continuous_preview_preserves_sampling_with_real_memory_guards(self):
        source = (ROOT / "src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")
        note = braced_block(source, "void WebHandlerContext::noteWebRequestActivity(")
        maintain = braced_block(source, "void WebHandlerContext::maintainWebForegroundTaskBudget()")
        cpp = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include "core/MemoryBudget.h"
uint32_t nowMs=1000, freeBytes=39000, largestBytes=18000;
bool psram=true;
unsigned long millis(){return nowMs;}
namespace FastBee { bool psramAvailableForJson(){return psram;} }
#define LOG_INFO(...) ((void)0)
struct HealthMonitor {bool severe=false;bool isMemorySevere(){return severe;}};
struct TaskManager {
 bool timer=true,network=true;unsigned enables=0,disables=0;uint32_t lastTimerRun=0;
 bool disableTask(const char* name){
   if(std::strcmp(name,"periph_exec_timer")==0){if(timer){timer=false;++disables;}return true;}
   network=false;return true;
 }
 bool enableTask(const char* name){
   if(std::strcmp(name,"periph_exec_timer")==0){if(!timer){timer=true;++enables;lastTimerRun=nowMs;}return true;}
   network=true;return true;
 }
};
struct MQTTClient {unsigned yields=0;void yieldToWeb(){++yields;}};
struct ProtocolManager {MQTTClient mqtt;MQTTClient* getMQTTClient(){return &mqtt;}};
struct FastBeeFramework {
 TaskManager task;HealthMonitor monitor;ProtocolManager protocol;
 static FastBeeFramework* getInstance(){static FastBeeFramework fw;return &fw;}
 TaskManager* getTaskManager(){return &task;}
 HealthMonitor* getHealthMonitor(){return &monitor;}
 ProtocolManager* getProtocolManager(){return &protocol;}
};
struct WebConfigManager {unsigned requests=0;void trackWebRequest(){++requests;}};
struct WebHandlerContext {
 unsigned long webForegroundUntilMs=0;bool webForegroundModeActive=false;
 WebConfigManager config;WebConfigManager* webConfigManager=&config;
 void noteWebRequestActivity(unsigned long holdMs=3500){NOTE}
 void maintainWebForegroundTaskBudget(){MAINTAIN}
};
int main(){
 auto& fw=*FastBeeFramework::getInstance();WebHandlerContext context;
 // Every 3 seconds a preview poll extends the 3.5 second hold. Acquisition
 // remains schedulable; repeated requests must not reset enabled task time.
 unsigned timerTicks=0;
 for(nowMs=1000;nowMs<=121000;nowMs+=1000){
   if((nowMs-1000)%3000==0)context.noteWebRequestActivity();
   context.maintainWebForegroundTaskBudget();
   assert(fw.task.timer && !fw.task.network);
   if(nowMs-fw.task.lastTimerRun>=10000){++timerTicks;fw.task.lastTimerRun=nowMs;}
 }
 assert(timerTicks==12 && fw.task.disables==0 && context.config.requests==41);
 assert(fw.protocol.mqtt.yields==41); // MQTT foreground yielding is retained
 // Real internal shortage must pause even with plenty of external memory.
 freeBytes=30719;context.noteWebRequestActivity();assert(!fw.task.timer);
 freeBytes=39000;largestBytes=8191;context.noteWebRequestActivity();assert(!fw.task.timer);
 largestBytes=18000;fw.monitor.severe=true;context.noteWebRequestActivity();assert(!fw.task.timer);
 fw.monitor.severe=false;context.noteWebRequestActivity();assert(fw.task.timer);
 assert(fw.task.enables==1); // recover during continuous polling, not only idle
 // No-PSRAM targets retain existing foreground protection.
 psram=false;context.noteWebRequestActivity();assert(!fw.task.timer);
 nowMs+=3500;context.maintainWebForegroundTaskBudget();
 assert(fw.task.timer && fw.task.network && !context.webForegroundModeActive);
 // When initially pressured, a capable target recovers during the next poll.
 psram=true;freeBytes=10000;context.noteWebRequestActivity();assert(!fw.task.timer);
 nowMs+=1000;freeBytes=39000;context.noteWebRequestActivity();assert(fw.task.timer);
}
'''.replace("NOTE", note).replace("MAINTAIN", maintain)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-web-sampling-") as directory:
            base = Path(directory)
            # MemoryBudget reads the same INTERNAL|8BIT values used on target.
            (base / "esp_heap_caps.h").write_text('''#pragma once
#include <cstdint>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
extern uint32_t freeBytes, largestBytes;
inline uint32_t heap_caps_get_free_size(int caps){return caps==3?freeBytes:4000000;}
inline uint32_t heap_caps_get_largest_free_block(int caps){return caps==3?largestBytes:3000000;}
''', encoding="utf-8")
            program = base / "sampling.cpp"
            program.write_text(cpp, encoding="utf-8")
            executable = base / ("sampling.exe" if os.name == "nt" else "sampling")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     "-DESP_PLATFORM=1", f"-I{base}", f"-I{ROOT / 'include'}",
                                     str(program), "-o", str(executable)],
                                    capture_output=True, text=True, timeout=40)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
