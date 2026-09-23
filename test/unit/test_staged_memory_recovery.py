"""Exercise real recovery ordering against changing internal-memory samples."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class StagedMemoryRecoveryTests(unittest.TestCase):
    def test_recheck_after_reclamation_and_critical_escalation(self):
        health = (ROOT / "src/systems/HealthMonitor.cpp").read_text(encoding="utf-8")
        recovery = braced_block(health, "void HealthMonitor::performMemoryRecovery(")
        gate = "if (oldLevel < MemoryGuardLevel::SEVERE ||"
        escalation = braced_block(health, gate)
        condition = health[health.index(gate):health.index("{", health.index(gate))]
        cpp = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
// Expected mutation failures must not launch Windows crash reporting or keep
// the temporary executable locked during cleanup.
#undef assert
#define assert(condition) do { if (!(condition)) { \
 std::fprintf(stderr, "check failed: %s\n", #condition); std::exit(2); \
} } while (false)
#define FASTBEE_ENABLE_MQTT 0
#define FASTBEE_ENABLE_MODBUS 1
#define FASTBEE_ENABLE_PERIPH_EXEC 1
#define pdMS_TO_TICKS(x) (x)
enum class MemoryGuardLevel {NORMAL,WARN,SEVERE,CRITICAL};
struct {template<class... T>void printf(const char*,T...){}void println(const char*){}} Serial;
uint32_t liveFree=22000,liveLargest=8000,modbusGain=2000,workerGain=10000;
uint32_t blockAfterWorkers=10000;
unsigned suspensionCalls=0;
bool workersSuspended=false,workerReleaseSucceeds=true;
namespace FastBee {
struct MemoryBudget {
 static uint32_t dram8Free(){return liveFree;}
 static uint32_t dram8Largest(){return liveLargest;}
};
struct RuntimeFeatureManager {
 static RuntimeFeatureManager& instance(){static RuntimeFeatureManager f;return f;}
 void setRuntimeDisabled(const char*,const char*){}
};
}
struct Handler {bool isRunning() const{return true;}};
struct Lease {Handler h;explicit operator bool()const{return true;}Handler* operator->(){return &h;}};
struct ProtocolManager {
 Lease acquireModbusHandler(int){return {};}
 void stopModbus(){liveFree+=modbusGain;}
};
struct FastBeeFramework {
 static FastBeeFramework* getInstance(){static FastBeeFramework f;return &f;}
 ProtocolManager* getProtocolManager(){static ProtocolManager p;return &p;}
};
struct PeriphExecScheduler {void setMemoryPaused(bool){}};
struct PeriphExecManager {
 static PeriphExecManager& getInstance(){static PeriphExecManager e;return e;}
 PeriphExecScheduler* getScheduler(){static PeriphExecScheduler s;return &s;}
 bool areWorkersSuspendedForMemory(){return workersSuspended;}
 bool suspendWorkersForMemory(){
   if(workerReleaseSucceeds){liveFree+=workerGain;liveLargest=blockAfterWorkers;workersSuspended=true;}
   return workerReleaseSucceeds;
 }
};
struct PeripheralManager {
 static PeripheralManager& getInstance(){static PeripheralManager p;return p;}
 size_t suspendOptionalForMemory(){++suspensionCalls;return 1;}
};
struct HealthMonitor {
 struct {uint32_t dramFreeHeap=22000,dramLargestBlock=8000;} currentHealth;
 bool _modbusStoppedForMemory=false,_periphExecPausedForMemory=false;
 bool _optionalPeripheralsSuspendedForMemory=false;
 uint16_t _memorySuspendedPeripheralCount=0;
 uint32_t guardSevereDramThreshold=24576,guardSevereLargestThreshold=4096;
 unsigned recoveryCalls=0;
 static uint8_t calculateHeapFragmentationPercent(uint32_t,uint32_t){return 60;}
 void ensureTLSContiguousMemory(){}
 void performMemoryRecovery(MemoryGuardLevel oldLevel,MemoryGuardLevel newLevel){
   (void)oldLevel;(void)newLevel;++recoveryCalls;RECOVERY
 }
 void transition(MemoryGuardLevel oldLevel,MemoryGuardLevel newLevel){CONDITION {ESCALATION}}
};
void reset(){liveFree=22000;liveLargest=8000;modbusGain=2000;workerGain=10000;
 blockAfterWorkers=10000;suspensionCalls=0;workersSuspended=false;workerReleaseSucceeds=true;}
int main(){
 // Stale health remains severe while service teardown restores live capacity.
 reset();HealthMonitor h;h.transition(MemoryGuardLevel::WARN,MemoryGuardLevel::SEVERE);
 assert(h.recoveryCalls==1 && liveFree==34000 && h.currentHealth.dramFreeHeap==22000);
 assert(suspensionCalls==0 && !h._optionalPeripheralsSuspendedForMemory);
 assert(h._modbusStoppedForMemory && h._periphExecPausedForMemory);
 // Stable severe samples do not repeat teardown or restore paused services.
 h.transition(MemoryGuardLevel::SEVERE,MemoryGuardLevel::SEVERE);
 assert(h.recoveryCalls==1 && suspensionCalls==0);
 // Later critical pressure must not be exempt just because entry was staged.
 liveFree=6000;liveLargest=2000;
 h.transition(MemoryGuardLevel::SEVERE,MemoryGuardLevel::CRITICAL);
 assert(h.recoveryCalls==2 && suspensionCalls==1 && h._optionalPeripheralsSuspendedForMemory);
 h.transition(MemoryGuardLevel::CRITICAL,MemoryGuardLevel::CRITICAL);
 assert(h.recoveryCalls==2 && suspensionCalls==1);
 // No release acknowledgement: retain full low-memory protection immediately.
 reset();h=HealthMonitor{};workerReleaseSucceeds=false;modbusGain=0;
 h.transition(MemoryGuardLevel::NORMAL,MemoryGuardLevel::CRITICAL);
 assert(suspensionCalls==1 && !workersSuspended && h._periphExecPausedForMemory);
 // Plenty of free bytes does not excuse a dangerously small contiguous block.
 reset();h=HealthMonitor{};blockAfterWorkers=4095;
 h.transition(MemoryGuardLevel::WARN,MemoryGuardLevel::SEVERE);assert(suspensionCalls==1);
 // Exact equality is sufficient; one byte below either target band is not.
 for(unsigned below=0;below<3;++below){
   reset();h=HealthMonitor{};modbusGain=0;workerGain=0;
   liveFree=24576-(below==1);blockAfterWorkers=4096-(below==2);
   h.transition(MemoryGuardLevel::WARN,MemoryGuardLevel::SEVERE);
   assert(suspensionCalls==(below?1U:0U));
 }
 // No-PSRAM targets use their own 12KB severe floor, not S3's 24KB floor.
 reset();h=HealthMonitor{};h.guardSevereDramThreshold=12288;
 liveFree=14000;modbusGain=0;workerGain=0;blockAfterWorkers=5000;
 h.transition(MemoryGuardLevel::WARN,MemoryGuardLevel::SEVERE);assert(suspensionCalls==0);
 // Already suspended devices cannot be counted/restarted in this stage.
 reset();h=HealthMonitor{};h._optionalPeripheralsSuspendedForMemory=true;
 workerReleaseSucceeds=false;modbusGain=0;liveFree=6000;
 h.transition(MemoryGuardLevel::SEVERE,MemoryGuardLevel::CRITICAL);assert(suspensionCalls==0);
}
'''.replace("RECOVERY", recovery).replace("CONDITION", condition).replace("ESCALATION", escalation)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-staged-recovery-") as directory:
            source = Path(directory) / "recovery.cpp"
            exe = Path(directory) / ("recovery.exe" if os.name == "nt" else "recovery")

            def run(program):
                source.write_text(program, encoding="utf-8")
                built = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                        str(source), "-o", str(exe)], capture_output=True, text=True, timeout=40)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                return subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)

            self.assertEqual(run(cpp).returncode, 0)
            stale = cpp.replace("const uint32_t remainingDram = FastBee::MemoryBudget::dram8Free();",
                                "const uint32_t remainingDram = currentHealth.dramFreeHeap;")
            self.assertNotEqual(stale, cpp, "Snapshot mutation must alter production code")
            result = run(stale)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("check failed: suspensionCalls==0 && !h._optionalPeripheralsSuspendedForMemory",
                          result.stderr, "Pre-recovery snapshot must fail the optional teardown check")
            no_retry = cpp.replace(condition, "if (oldLevel < MemoryGuardLevel::SEVERE)")
            self.assertNotEqual(no_retry, cpp, "Escalation mutation must alter production code")
            result = run(no_retry)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("check failed: h.recoveryCalls==2 && suspensionCalls==1 && h._optionalPeripheralsSuspendedForMemory",
                          result.stderr, "Critical escalation must fail the second recovery check")


if __name__ == "__main__":
    unittest.main()
