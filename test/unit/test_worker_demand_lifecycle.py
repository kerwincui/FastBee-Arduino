"""Compile production worker demand and lifecycle, including concurrent admission."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class WorkerDemandLifecycleTests(unittest.TestCase):
    def test_production_demand_suspend_recovery_and_concurrent_creation(self):
        source = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf8')
        source += '\n' + (ROOT / 'src/core/PeriphExecWorkers.cpp').read_text(encoding='utf8')
        production = '\n'.join(definition(source, signature) for signature in (
            'bool PeriphExecManager::shouldAvoidSyncFallback(',
            'bool PeriphExecManager::ruleRequiresWorkerPool(',
            'bool PeriphExecManager::hasWorkerPoolDemandLocked(',
            'bool PeriphExecManager::ensureWorkerPoolForRule(',
            'bool PeriphExecManager::suspendWorkersForMemory(',
            'bool PeriphExecManager::resumeWorkersAfterMemory(',
        ))
        enums = definition((ROOT / 'include/core/PeripheralExecution.h').read_text(encoding='utf8'),
                           'enum class ExecActionType') + ';'
        predicates = (ROOT / 'include/core/NativeOutputDispatch.h').read_text(encoding='utf8')
        harness = r'''
#include <cassert>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
struct String : std::string { using std::string::string; bool isEmpty()const{return empty();} };
#define FASTBEE_WORKER_BUDGETED 0
bool CHIP_IS_CLASSIC_ESP32=true;
int FASTBEE_USE_PSRAM=0;
constexpr unsigned MAX_ACTIONS_PER_RULE=4;
enum class PeripheralType {GPIO_DIGITAL_OUTPUT=12,GPIO_ANALOG_OUTPUT=16,GPIO_PWM_OUTPUT=17,DAC=18,OTHER=99};
struct PeripheralConfig {PeripheralType type=PeripheralType::OTHER;bool modbus=false,expander=false;
    struct {struct {bool passive=false;} gpio;} params;
    bool isModbusPeripheral()const{return modbus;}bool isExpanderChannel()const{return expander;}};
struct PeripheralManager {std::map<String,PeripheralConfig> targets;
    static PeripheralManager& getInstance(){static PeripheralManager pm;return pm;}
    bool getPeripheralSnapshot(const String& id,PeripheralConfig& cfg){auto it=targets.find(id);
        if(it==targets.end())return false;
        cfg=it->second;return true;}};
struct ExecAction {String targetPeriphId;uint8_t actionType=10;uint16_t syncDelayMs=0;};
struct PeriphExecRule {bool enabled=true;std::vector<ExecAction> actions;};
struct Log {void info(const char*){}void warning(const char*){}} LOGGER;
std::atomic<int> created{0}; bool headroom=true, beginOk=true, shutdownOk=true;
bool hasWorkerPoolMemoryHeadroom(bool){return headroom;}
struct PeriphExecWorkerPool {static constexpr unsigned WORKER_COUNT=1;bool started=false;
    bool begin(unsigned=WORKER_COUNT){++created;started=beginOk;return started;}
    bool shutdown(){if(!shutdownOk)return false;started=false;return true;}
    bool isStarted(){return started;}};
struct MutexGuard {std::unique_lock<std::timed_mutex> guard;
    explicit MutexGuard(std::timed_mutex* mutex):guard(*mutex,std::defer_lock){(void)guard.try_lock_for(std::chrono::milliseconds(100));}
    bool isLocked()const{return guard.owns_lock();}};
struct PeriphExecManager {std::timed_mutex mutex;std::timed_mutex* _rulesMutex=&mutex;
    std::map<String,PeriphExecRule> rules;
    std::unique_ptr<PeriphExecWorkerPool> _workerPool;
    bool _workersSuspendedForMemory=false,_workerSuspensionPending=false,_workerPoolDemandPending=false;
    bool shouldAvoidSyncFallback(const PeriphExecRule&)const;
    bool ruleRequiresWorkerPool(const PeriphExecRule&)const;
    bool hasWorkerPoolDemandLocked()const;
    bool ensureWorkerPoolForRule(const PeriphExecRule&);
    bool suspendWorkersForMemory();bool resumeWorkersAfterMemory();};
'''
        harness += enums + '\nnamespace FastBee {\n'
        harness += definition(predicates, 'inline bool isOrdinaryDigitalTarget(') + '\n'
        harness += definition(predicates, 'inline bool isOrdinaryScalarTarget(') + '\n}\n'
        harness += production + r'''
int main(){
    auto& pm=PeripheralManager::getInstance();
    for(const char* id:{"relay","led_d1","led_d2","led_d3","led_d4"})
        pm.targets[id].type=PeripheralType::GPIO_DIGITAL_OUTPUT;
    pm.targets["buzzer"].type=PeripheralType::GPIO_PWM_OUTPUT;
    // COM16's six configured rules: relay/PWM toggle/value and four-LED batches.
    PeriphExecRule relay{true,{{"relay",10,0}}}, buzzer{true,{{"buzzer",10,0}}};
    PeriphExecRule leds{true,{{"led_d1",10,0},{"led_d2",10,0},{"led_d3",10,0},{"led_d4",10,0}}};
    PeriphExecManager m;
    m.rules={{"exec_005",relay},{"exec_006",buzzer},{"exec_009",leds},
             {"exec_012",relay},{"exec_013",buzzer},{"exec_015",leds}};
    assert(!m.hasWorkerPoolDemandLocked());
    headroom=false;
    for(const auto& entry:m.rules)assert(m.ensureWorkerPoolForRule(entry.second));
    assert(!m._workerPool && !m._workersSuspendedForMemory && created==0);
    assert(m.suspendWorkersForMemory());assert(m._workersSuspendedForMemory);
    assert(m.resumeWorkersAfterMemory());assert(!m._workersSuspendedForMemory && !m._workerPool);
    auto heavy=relay;heavy.actions[0].actionType=19;
    assert(m.ruleRequiresWorkerPool(heavy));
    auto wait=relay;wait.actions[0].syncDelayMs=1;assert(m.ruleRequiresWorkerPool(wait));
    auto script=relay;script.actions[0].actionType=15;assert(m.ruleRequiresWorkerPool(script));
    auto blink=relay;blink.actions[0].actionType=2;assert(m.ruleRequiresWorkerPool(blink));
    pm.targets["relay"].expander=true;assert(m.ruleRequiresWorkerPool(relay));pm.targets["relay"].expander=false;
    pm.targets["relay"].modbus=true;assert(m.ruleRequiresWorkerPool(relay));pm.targets["relay"].modbus=false;
    auto unknown=relay;unknown.actions[0].targetPeriphId="unknown";assert(m.ruleRequiresWorkerPool(unknown));
    auto overflow=leds;overflow.actions.push_back({"relay",10,0});assert(m.ruleRequiresWorkerPool(overflow));
    // Disabled heavy config does not wake a pool, but a manual request does.
    heavy.enabled=false;m.rules["heavy"]=heavy;assert(!m.hasWorkerPoolDemandLocked());
    assert(!m.ensureWorkerPoolForRule(heavy));assert(m._workersSuspendedForMemory && m._workerPoolDemandPending);
    assert(!m.resumeWorkersAfterMemory());assert(!m._workerPool && created==0);
    headroom=true;beginOk=false;assert(!m.resumeWorkersAfterMemory());
    assert(!m._workerPool && m._workersSuspendedForMemory);
    beginOk=true;assert(m.resumeWorkersAfterMemory());
    assert(m._workerPool && m._workerPool->isStarted() && !m._workerPoolDemandPending);
    auto* active=m._workerPool.get();assert(m.ensureWorkerPoolForRule(relay));assert(m._workerPool.get()==active);
    shutdownOk=false;assert(!m.suspendWorkersForMemory());assert(m._workerPool.get()==active && m._workerSuspensionPending);
    assert(!m.ensureWorkerPoolForRule(heavy));assert(!m.resumeWorkersAfterMemory());assert(m._workerPool.get()==active);
    shutdownOk=true;assert(m.suspendWorkersForMemory());assert(!m._workerPool);
    assert(m.resumeWorkersAfterMemory());assert(m._workerPool);
    PeriphExecManager concurrent;created=0;
    std::vector<std::thread> callers;
    for(int i=0;i<8;++i)callers.emplace_back([&]{assert(concurrent.ensureWorkerPoolForRule(heavy));});
    for(auto& thread:callers)thread.join();
    assert(created==1);
    // Profile behavior is preserved outside classic no-PSRAM.
    CHIP_IS_CLASSIC_ESP32=false;PeriphExecManager c3;assert(c3.hasWorkerPoolDemandLocked());
    assert(!c3.ruleRequiresWorkerPool(wait));assert(c3.ruleRequiresWorkerPool(script));
    CHIP_IS_CLASSIC_ESP32=true;FASTBEE_USE_PSRAM=1;assert(c3.hasWorkerPoolDemandLocked());
    assert(!c3.ruleRequiresWorkerPool(wait));
}
'''
        compiler = shutil.which('g++')
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix='fastbee-worker-demand-') as directory:
            cpp, exe = Path(directory) / 'demand.cpp', Path(directory) / 'demand.exe'
            cpp.write_text(harness, encoding='utf8')
            compiled = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                       '-pthread', str(cpp), '-o', str(exe)], capture_output=True,
                                      text=True, env=os.environ.copy(), timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_dispatch_checks_worker_admission_before_operation_or_reservation(self):
        source = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf8')
        source += '\n' + (ROOT / 'src/core/PeriphExecWorkers.cpp').read_text(encoding='utf8')
        dispatch = definition(source, 'bool PeriphExecManager::dispatchAsync(')
        demand = dispatch.index('ensureWorkerPoolForRule(rule)')
        self.assertLess(demand, dispatch.index('_runningRuleIds.count'))
        self.assertLess(demand, dispatch.index('beginOperation('))
        self.assertIn('"worker_memory_pending"', dispatch)
        self.assertNotIn('shouldAvoidSyncFallback(rule)', dispatch)


if __name__ == '__main__':
    unittest.main()
