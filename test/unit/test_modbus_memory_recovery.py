"""Execute the real Modbus recovery request/confirmation sequence."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class ModbusMemoryRecoveryTests(unittest.TestCase):
    def test_normal_samples_preserve_confirmation_idle_and_retry_deadline(self):
        health = (ROOT / "src/systems/HealthMonitor.cpp").read_text(encoding="utf-8")
        protocol = (ROOT / "src/protocols/ProtocolManager.cpp").read_text(encoding="utf-8")
        restore = braced_block(health, "void HealthMonitor::restoreMemoryRecovery()")
        begin = restore.index("#if FASTBEE_ENABLE_MODBUS")
        restore = restore[begin:restore.index("#endif", begin) + len("#endif")]
        reconcile = braced_block(health, "void HealthMonitor::reconcilePendingMemoryRecovery()")
        deferred = braced_block(protocol, "bool ProtocolManager::restartModbusDeferred()")
        consume = braced_block(protocol, "bool ProtocolManager::consumeModbusRestartResult(")
        cpp = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
// Mutation failures are expected test outcomes, not OS crash reports. Native
// assert may launch Windows error reporting and keep the temporary exe locked.
#undef assert
#define assert(condition) do { if (!(condition)) { \
 std::fprintf(stderr, "check failed: %s\n", #condition); std::exit(2); \
} } while (false)
#define FASTBEE_ENABLE_MODBUS 1
#define FASTBEE_ENABLE_PERIPH_EXEC 0
#define FASTBEE_ENABLE_MQTT 0
#define pdMS_TO_TICKS(value) (value)
#define LOG_INFO(...) ((void)0)
unsigned long nowMs=1000;
unsigned long millis(){return nowMs;}
struct {void println(const char*){}} Serial;
constexpr unsigned long MODBUS_MEMORY_RETRY_MS=30000;
enum class ModbusRestartResult{Failed,Started,IdleNoEnabledTargets,DeferredBusy};
struct ModbusLifecycleGuard{ModbusLifecycleGuard(bool,int){}bool isLocked()const{return true;}};
struct Handler{bool running=false;bool isRunning()const{return running;}};
struct Lease{Handler* p;explicit operator bool()const{return p!=nullptr;}Handler* operator->(){return p;}};
struct ProtocolManager{
 bool modbusLifecycleMutex=true,modbusShutdownPending=false,modbusStopPending=false;
 bool modbusRestartPending=false,modbusRestartResultAvailable=false;
 ModbusRestartResult modbusLastRestartResult=ModbusRestartResult::Failed;
 Handler handler;unsigned queues=0;
 bool restartModbusDeferred(){++queues;DEFERRED}
 bool consumeModbusRestartResult(ModbusRestartResult& result){CONSUME}
 bool isModbusRestartPending()const{return modbusRestartPending;}
 Lease acquireModbusHandler(int){return {modbusRestartPending||modbusStopPending?nullptr:&handler};}
 void complete(ModbusRestartResult result){modbusRestartPending=false;modbusLastRestartResult=result;
  modbusRestartResultAvailable=true;handler.running=result==ModbusRestartResult::Started;}
} protocol;
struct FastBeeFramework{
 static FastBeeFramework* getInstance(){static FastBeeFramework f;return &f;}
 ProtocolManager* getProtocolManager(){return &protocol;}
};
namespace FastBee{struct RuntimeFeatureManager{
 static RuntimeFeatureManager& instance(){static RuntimeFeatureManager f;return f;}
 void clearRuntimeDisabled(const char*){}
};}
struct HealthMonitor{
 bool _modbusStoppedForMemory=true,_modbusRestorePendingForMemory=false;
 unsigned long _modbusRestoreRetryAtMs=0;
 void restore(){auto* pm=&protocol;
RESTORE
 }
 void reconcilePendingMemoryRecovery(){RECONCILE}
 void healthSample(){restore();reconcilePendingMemoryRecovery();}
};
void reset(){protocol=ProtocolManager{};nowMs=1000;}
int main(){
 // Every health cycle invokes restore before confirmation, as on the device.
 reset();HealthMonitor h;h.healthSample();
 assert(protocol.queues==1&&protocol.modbusRestartPending);
 assert(h._modbusStoppedForMemory&&h._modbusRestorePendingForMemory);
 auto deadline=h._modbusRestoreRetryAtMs;
 nowMs+=5000;h.healthSample();
 assert(protocol.queues==1&&h._modbusRestoreRetryAtMs==deadline);
 protocol.complete(ModbusRestartResult::Started);nowMs+=5000;h.healthSample();
 assert(protocol.queues==1&&!h._modbusStoppedForMemory&&!h._modbusRestorePendingForMemory);
 assert(h._modbusRestoreRetryAtMs==0);h.healthSample();assert(protocol.queues==1);
 // Intentional idle is a completed recovery even without a running handler.
 reset();h=HealthMonitor{};h.healthSample();protocol.complete(ModbusRestartResult::IdleNoEnabledTargets);
 h.healthSample();assert(protocol.queues==1&&!h._modbusStoppedForMemory);
 // Failed restart stays degraded, preserving its original retry deadline.
 reset();h=HealthMonitor{};h.healthSample();deadline=h._modbusRestoreRetryAtMs;
 protocol.complete(ModbusRestartResult::Failed);
 for(nowMs=6000;nowMs<deadline;nowMs+=5000){h.healthSample();
  assert(protocol.queues==1&&h._modbusStoppedForMemory&&h._modbusRestoreRetryAtMs==deadline);}
 nowMs=deadline;h.healthSample();assert(protocol.queues==2&&protocol.modbusRestartPending);
 assert(h._modbusRestorePendingForMemory&&h._modbusStoppedForMemory);
 protocol.complete(ModbusRestartResult::Started);h.healthSample();assert(!h._modbusStoppedForMemory);
 // A rejected initial queue is retried only when its deadline arrives.
 reset();h=HealthMonitor{};protocol.modbusShutdownPending=true;h.healthSample();
 assert(protocol.queues==1&&!protocol.modbusRestartPending&&h._modbusStoppedForMemory);
 nowMs+=5000;h.healthSample();assert(protocol.queues==1);
 protocol.modbusShutdownPending=false;nowMs=h._modbusRestoreRetryAtMs;h.healthSample();
 assert(protocol.queues==2&&protocol.modbusRestartPending&&h._modbusStoppedForMemory);
 // An active lease/deferred request is not replaced after its deadline.
 nowMs+=60000;h.healthSample();assert(protocol.queues==2&&h._modbusStoppedForMemory);
}
'''.replace("DEFERRED", deferred).replace("CONSUME", consume).replace("RESTORE", restore).replace("RECONCILE", reconcile)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-modbus-recovery-") as directory:
            base = Path(directory)
            source = base / "recovery.cpp"
            executable = base / ("recovery.exe" if os.name == "nt" else "recovery")
            source.write_text(cpp, encoding="utf-8")
            compiled = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                       str(source), "-o", str(executable)],
                                      capture_output=True, text=True, timeout=40)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # The original unconditional requeue must fail this exact sequence.
            mutant = cpp.replace("_modbusStoppedForMemory && !_modbusRestorePendingForMemory",
                                 "_modbusStoppedForMemory")
            self.assertNotEqual(mutant, cpp, "Mutation must alter the recovery admission condition")
            source.write_text(mutant, encoding="utf-8")
            compiled = subprocess.run([compiler, "-std=c++17", str(source), "-o", str(executable)],
                                      capture_output=True, text=True, timeout=40)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
            self.assertIn("check failed: protocol.queues==1&&h._modbusRestoreRetryAtMs==deadline",
                          result.stderr, "Original requeue bug must fail its specific assertion")


if __name__ == "__main__":
    unittest.main()
