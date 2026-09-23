"""Execute the production two-phase screen recovery with lock-aware fakes."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class ScreenMemoryRecoveryTests(unittest.TestCase):
    def test_runtime_start_is_unlocked_and_failed_recovery_stays_pending(self):
        manager = (ROOT / "src/core/PeripheralManager.cpp").read_text(encoding="utf-8")
        body = braced_block(manager, "size_t PeripheralManager::restoreOptionalAfterMemory()")
        # Compile the real admission guards, not a second implementation.
        guards = []
        for signature, args in [
            ("bool PeripheralManager::disablePeripheral(const String& id)", "const String& id"),
            ("bool PeripheralManager::removePeripheral(const String& id)", "const String& id"),
            ("bool PeripheralManager::enablePeripheral(const String& id)", "const String& id"),
            ("bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg)", "const String& id,const PeripheralConfig& config,String& errorMsg"),
            ("bool PeripheralManager::initHardware(const String& id)", "const String& id"),
            ("bool PeripheralManager::deinitHardware(const String& id)", "const String& id"),
        ]:
            method = signature.split("::")[1].split("(")[0]
            source = braced_block(manager, signature)
            begin = source.index("if (!_memoryRestoringDisplayId.isEmpty()")
            end = source.find("\n    auto ", begin)
            if method == "removePeripheral":
                end = source.index("\n    if (!hasPeripheral", begin)
            if method == "enablePeripheral":
                end = source.index("\n    auto config", begin)
            guards.append(f"bool guard_{method}({args}) {{ RecursiveMutexGuard lock(_mutex); {source[begin:end]} return true; }}")
        cpp = r'''
#include <cassert>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
class String:public std::string{public:using std::string::string;using std::string::operator=;bool isEmpty()const{return empty();}};
int lockDepth=0;
struct RecursiveMutexGuard{explicit RecursiveMutexGuard(bool){++lockDepth;}~RecursiveMutexGuard(){--lockDepth;}};
#define LOG_INFOF(...) ((void)0)
#define FASTBEE_ENABLE_SCREEN_PANEL 1
enum class PeripheralType{SCREEN_DISPLAY,OTHER};
enum class PeripheralStatus{PERIPHERAL_DISABLED,PERIPHERAL_ENABLED,PERIPHERAL_INITIALIZED};
struct PeripheralConfig{PeripheralType type=PeripheralType::SCREEN_DISPLAY;bool enabled=false;};
struct State{bool memorySuspended=true;PeripheralStatus status=PeripheralStatus::PERIPHERAL_DISABLED;String lastError;};
namespace FastBee{namespace Screen{
struct ScreenRuntime{
 bool succeed=true;bool running=false;unsigned starts=0,stops=0;std::function<void()> duringStart;
 static ScreenRuntime& instance(){static ScreenRuntime r;return r;}
 bool start(String& error){assert(lockDepth==0);++starts;if(duringStart)duringStart();running=succeed;error=succeed?"":"SCREEN_RUNTIME_TASK_ALLOCATION_FAILED";return succeed;}
 bool stop(){assert(lockDepth==0);++stops;running=false;return true;}
};}}
struct ScreenPanelManager{
 bool applied=false,restartRequired=true;String error;
 static ScreenPanelManager& instance(){static ScreenPanelManager p;return p;}
 void reportApplication(bool value,bool restart,const String& detail){assert(lockDepth==0);applied=value;restartRequired=restart;error=detail;}
};
struct PeripheralManager{
 bool _mutex=true,initSucceeds=true;size_t _memorySuspendedCount=1;String _memoryRestoringDisplayId,lastEnableError;
 std::map<String,PeripheralConfig> peripherals;
 std::map<String,State> runtimeStates;
 void invalidateSensorConfigurationLocked(){}
 bool initHardware(const String& id){assert(lockDepth>0);return initSucceeds && guard_initHardware(id);}
 size_t restoreOptionalAfterMemory();
 GUARDS
};
size_t PeripheralManager::restoreOptionalAfterMemory(){BODY}
int main(){
 PeripheralManager m;m.peripherals["panel"]={};m.runtimeStates["panel"]={};
 auto& runtime=FastBee::Screen::ScreenRuntime::instance();auto& panel=ScreenPanelManager::instance();
 runtime.duringStart=[&]{
   assert(m._memorySuspendedCount==1 && m.runtimeStates["panel"].memorySuspended);
   assert(m.peripherals["panel"].enabled && m._memoryRestoringDisplayId=="panel");
   assert(!m.guard_disablePeripheral("panel") && !m.guard_removePeripheral("panel"));
   assert(!m.guard_enablePeripheral("panel") && !m.guard_initHardware("panel") && !m.guard_deinitHardware("panel"));
   String error;assert(!m.guard_updatePeripheral("panel",m.peripherals["panel"],error));
   assert(error=="SCREEN_MEMORY_RESTORE_IN_PROGRESS");
   assert(m.guard_disablePeripheral("other"));
   assert(m.restoreOptionalAfterMemory()==0); // second recovery cannot race
 };
 runtime.succeed=false;
 assert(m.restoreOptionalAfterMemory()==0 && !runtime.running);
 assert(m._memorySuspendedCount==1 && m.runtimeStates["panel"].memorySuspended);
 assert(!m.peripherals["panel"].enabled && m._memoryRestoringDisplayId.empty());
 assert(m.runtimeStates["panel"].lastError.find("memory_pressure_restore_pending")==0);
 assert(!panel.applied && !panel.restartRequired && !panel.error.empty());
 runtime.succeed=true;
 assert(m.restoreOptionalAfterMemory()==1 && runtime.running);
 assert(m._memorySuspendedCount==0 && !m.runtimeStates["panel"].memorySuspended);
 assert(m.peripherals["panel"].enabled && panel.applied && !panel.restartRequired && panel.error.empty());
 assert(m.restoreOptionalAfterMemory()==0 && runtime.starts==2);
 assert(m.guard_disablePeripheral("panel")); // user operation allowed after start
 // Hardware configuration failure never attempts runtime startup.
 m.runtimeStates["panel"].memorySuspended=true;m._memorySuspendedCount=1;m.initSucceeds=false;
 assert(m.restoreOptionalAfterMemory()==0 && runtime.starts==2 && m._memorySuspendedCount==1);
 // Non-display peripherals retain their existing immediate recovery behavior.
 m.initSucceeds=true;m.peripherals["panel"].type=PeripheralType::OTHER;
 assert(m.restoreOptionalAfterMemory()==1 && m._memorySuspendedCount==0 && runtime.starts==2);
 assert(lockDepth==0);
}
'''.replace("GUARDS", "\n".join(guards)).replace("BODY", body)
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="fastbee-screen-recovery-") as directory:
            source = Path(directory) / "recovery.cpp"
            source.write_text(cpp, encoding="utf-8")
            exe = Path(directory) / ("recovery.exe" if os.name == "nt" else "recovery")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(exe)], capture_output=True, text=True, timeout=40)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
