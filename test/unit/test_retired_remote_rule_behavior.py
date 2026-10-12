"""Execute production rule retirement admission and motor-result event routing."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(signature)


class RetiredRemoteRuleBehaviorTests(unittest.TestCase):
    def test_production_admission_and_motor_feedback_routing(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("C++17 compiler required")
        json_headers = list(Path(os.environ.get('PLATFORMIO_LIBDEPS_DIR',str(ROOT/'.pio/libdeps'))).glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Existing ArduinoJson headers required")
        source = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
        source += "\n" + (ROOT / "src/core/PeriphExecRuleQueries.cpp").read_text(encoding="utf-8")
        source += "\n" + (ROOT / "include/core/PeriphExecRemoteBindings.h").read_text(encoding="utf-8")
        production = "\n".join(method(source, signature) for signature in (
            "bool hasRetiredRemoteSource(",
            "bool PeriphExecManager::sanitizeRuleForSafety(",
            "bool PeriphExecManager::getRuleSnapshot(",
            "bool PeriphExecManager::isRuleExecutionCurrent(",
            "bool PeriphExecManager::enableRule(",
            "bool PeriphExecManager::setRuleEnabledLocked(",
            "bool PeriphExecManager::runOnce(",
            "bool PeriphExecManager::dispatchAsync(",
            "void PeriphExecManager::dispatchEventMatchedRules(",
            "void PeriphExecManager::dispatchPeriphExecEvent(",
        ))
        harness = r'''
#include <Arduino.h>
#include "network/LiteralStringMatch.h"
#include <ArduinoJson.h>
#include "core/PeripheralExecution.h"
#include "core/RuleCompletionGraph.h"
#include "core/ThermostatConfig.h"
#include "core/SdRecorderConfig.h"
#include "core/UartEventOrigin.h"
#include "core/NativeOutputControl.h"
#include "core/RuleOutputIdentity.h"
#include "core/SensorConfigEpoch.h"
#include <cassert>
#include <set>
inline void convertFromJson(JsonVariantConst value,String& out) { out=value.as<const char*>(); }
struct Logger {
    template<class... T> void warning(T...) {}
    template<class... T> void warningf(T...) {}
    template<class... T> void infof(T...) {}
    template<class... T> void errorf(T...) {}
} LOGGER;
#define LOG_DEBUGF(...) ((void)0)
constexpr int pdTRUE=1;
struct MutexGuard { explicit MutexGuard(int,int=0) {} bool isLocked() const { return true; } };
int xSemaphoreTake(int,int) { return pdTRUE; }
void xSemaphoreGive(int) {}
int uxSemaphoreGetCount(int) { return 1; }
namespace FastBee { namespace MemoryBudget { size_t dramFree() { return 100000; } } }
struct HealthMonitor { bool isMemorySevere() const { return false; } };
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { return nullptr; }
    HealthMonitor* getHealthMonitor() { return nullptr; }
};
enum class AsyncExecStatus { PENDING,REJECTED };
enum class AsyncExecError { NONE,WORKER_QUEUE_FULL,UART_SOURCE_UNCONFIRMED };
struct PeripheralManager {
    bool acceptCaptured = false;
    uint64_t nextControlOperation = 0;
    // Admission fixture only: output writes are tested by the real PM arbiter.
    bool beginOrdinaryControl(FastBee::OrdinaryControl::Source source, uint64_t actor,
        uint32_t holdMs, bool overrideConfirmed, uint64_t reviewedGeneration,
        const String& target, uint32_t epoch, const FastBee::UartEventOrigin& origin,
        FastBee::OrdinaryControlContext& context) {
        assert(!holdMs && !overrideConfirmed && !reviewedGeneration && target.isEmpty());
        context={};
        if (!actor || !FastBee::SensorConfigEpoch::matches(epoch) ||
            !isUartEventOriginCurrent(origin)) return false;
        context.request.actor.source=source; context.request.actor.identity=actor;
        context.request.operation=++nextControlOperation;
        context.configEpoch=epoch; context.uartOrigin=origin;
        return true;
    }
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
        return origin.unbound() || (acceptCaptured && origin.matches(7,9));
    }
};
struct AsyncExecResult {
    FastBee::UartEventOrigin uartOrigin;
    FastBee::OrdinaryControlContext ordinaryControl;
    unsigned operationId=0,queuedAt=0,deadlineAt=0,endTime=0,peripheralConfigEpoch=0;
    uint8_t queuePosition=0; bool asyncExecution=false;
    AsyncExecStatus status=AsyncExecStatus::PENDING;
    AsyncExecError errorCode=AsyncExecError::NONE;
};
bool configError(String& error,const char* issue) { error=issue; return false; }
struct PeriphExecManager;
struct AsyncExecContext {
    FastBee::UartEventOrigin uartOrigin;
    FastBee::OrdinaryControlContext ordinaryControl;
    PeriphExecRule ruleCopy; String receivedValue; PeriphExecManager* manager=nullptr;
    void* mqtt=nullptr; int taskSlot=0;
    unsigned operationId=0,queuedAt=0,deadlineAt=0,peripheralConfigEpoch=0; uint8_t queuePosition=0;
};
struct Pool {
    AsyncExecContext entry;
    AsyncExecContext* acquire() { return &entry; }
    void release(AsyncExecContext*) {}
};
struct PeriphExecWorkerPool {
    bool isStarted() const { return false; }
    size_t pendingCount() const { return 0; }
    bool enqueue(AsyncExecContext*) { return false; }
    static size_t capacity() { return 1; }
};
bool isTerminalSystemAction(uint8_t action) { return action==6; }
struct PeriphExecManager {
    int _rulesMutex=1,_runningRulesMutex=2,_taskSlotSemaphore=3;
    std::map<String,PeriphExecRule> rules;
    std::set<String> _runningRuleIds;
    std::map<String,unsigned long> _runningStartTime,_failureBackoff;
    Pool _contextPool; PeriphExecWorkerPool* _workerPool=nullptr;
    int executions=0,admissions=0,refreshes=0,workerAdmissions=0;
    mutable int workerDemandChecks=0;
    bool allowWorkers=true;
    uint64_t _ruleRuntimeRevision=1;
    bool _configurationLoadFailed=false,_configurationRestartPending=false;
    std::vector<String> matched;
    std::vector<FastBee::UartEventOrigin> origins;
    const char* configurationWriteError() const { return nullptr; }
    bool ruleHasPollCollectionAction(const PeriphExecRule&) const { return false; }
    bool ruleNeedsModbus(const PeriphExecRule&) const { return false; }
    bool sanitizeTriggerForSafety(ExecTrigger&,bool,const String&) const { return false; }
    void refreshRuleRuntimeIndexesLocked() { ++refreshes; }
    bool sanitizeRuleForSafety(PeriphExecRule&) const;
    bool getRuleSnapshot(const String&,PeriphExecRule&) const;
    bool isRuleExecutionCurrent(const PeriphExecRule&) const;
    bool validateRuleActionsLocked(const PeriphExecRule& rule,String& error) const {
        return FastBee::validateThermostatRule(rule,error);
    }
    bool enableRule(const String&);
    bool setRuleEnabledLocked(const String&,bool,String&);
    bool runOnce(const String&,const String&,String*,AsyncExecResult*,
                 const FastBee::OrdinaryControlContext* = nullptr);
    bool dispatchAsync(const PeriphExecRule&,const String&,String*,AsyncExecResult*,
        const FastBee::UartEventOrigin& = {}, const FastBee::OrdinaryControlContext* = nullptr,
        FastBee::OrdinaryControl::Source = FastBee::OrdinaryControl::Source::Rule);
    void dispatchEventMatchedRules(const String&,const String&,const FastBee::UartEventOrigin& = {});
    void dispatchPeriphExecEvent(const String&,const String&,const FastBee::UartEventOrigin& = {});
    bool ruleRequiresWorkerPool(const PeriphExecRule&) const { ++workerDemandChecks; return false; }
    bool ensureWorkerPoolForRule(const PeriphExecRule&) { ++workerAdmissions; return allowWorkers; }
    bool shouldRunAsync() const { return false; }
    bool beginOperation(const PeriphExecRule&,bool,uint8_t,AsyncExecResult& result,
        const FastBee::UartEventOrigin&,const FastBee::OrdinaryControlContext& control) {
        result.ordinaryControl=control;
        result.operationId=++admissions; return true;
    }
    void executeSyncWithCompletion(const PeriphExecRule& rule,const String&,AsyncExecResult&) {
        executions+=rule.actions.size();
        _runningRuleIds.erase(rule.id); _runningStartTime.erase(rule.id);
    }
    void recordResult(const AsyncExecResult&) {}
    bool dispatchAndCommitTrigger(const String& id,size_t,const String&,const char*,const FastBee::UartEventOrigin& origin = {},uint64_t revision=0) {
        assert(revision && rules.at(id).runtimeRevision==revision);
        matched.push_back(id); origins.push_back(origin); return true;
    }
};
'''
        checks = r'''
ExecAction action(uint8_t type,const char* target="",const char* value="") {
    ExecAction result; result.actionType=type; result.targetPeriphId=target; result.actionValue=value; return result;
}
PeriphExecRule basicRule(const char* id) {
    PeriphExecRule rule; rule.id=id; rule.name=id; rule.enabled=true;rule.runtimeRevision=1;
    rule.outputActor=FastBee::RuleOutputIdentity::allocateActor();
    rule.actions={action(0,"gpio-first"),action(1,"gpio-last")}; return rule;
}
void checkRetired(bool byTrigger) {
    PeriphExecManager manager;
    auto rule=basicRule("legacy");
    if (byTrigger) {
        ExecTrigger trigger; trigger.triggerPeriphId="remote.lamp"; rule.triggers.push_back(trigger);
    } else rule.actions.insert(rule.actions.begin()+1,action(33,"lamp","on"));
    const size_t originalActions=rule.actions.size();
    assert(manager.sanitizeRuleForSafety(rule));
    assert(!rule.enabled && rule.actions.size()==originalActions);
    assert(!manager.sanitizeRuleForSafety(rule)); // Idempotent startup migration.
    manager.rules[rule.id]=rule;
    assert(!manager.enableRule(rule.id) && !manager.rules[rule.id].enabled);
    assert(manager.refreshes==0);
    String reason; AsyncExecResult admitted; admitted.operationId=99;
    assert(!manager.runOnce(rule.id,"",&reason,&admitted));
    assert(reason==(byTrigger ? "remote_source_retired":"remote_channel_retired"));
    assert(admitted.operationId==0 && manager.executions==0 && manager.admissions==0);
    rule.enabled=true; // Even a stale/imported enabled snapshot cannot bypass admission.
    assert(!manager.dispatchAsync(rule,"",&reason,&admitted));
    assert(manager.executions==0 && manager._runningRuleIds.empty());
    assert(manager.workerDemandChecks==0 && manager.workerAdmissions==0);
    assert(rule.actions.front().targetPeriphId=="gpio-first");
}
int main() {
    checkRetired(false); checkRetired(true);
    {
        PeriphExecManager manager; auto rule=basicRule("child");
        ExecTrigger trigger; trigger.triggerType=static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER);
        trigger.eventId="exec_parent"; rule.triggers.push_back(trigger); manager.rules[rule.id]=rule;
        const auto origin=FastBee::UartEventOrigin::captured(7,9);
        manager.dispatchEventMatchedRules("exec_parent","value",origin);
        manager.dispatchPeriphExecEvent("exec_parent","cancelled",origin);
        assert(manager.matched.empty()); // Retired source cannot create unbound descendants.
        PeripheralManager::getInstance().acceptCaptured=true;
        manager.dispatchEventMatchedRules("exec_parent","value",origin);
        manager.dispatchPeriphExecEvent("exec_parent","failed",origin);
        assert(manager.matched.size()==2 && manager.origins.size()==2);
        for(const auto& source:manager.origins) assert(source.matches(7,9));
        assert(manager.rules[rule.id].triggers[0].triggerCount==0); // Stub admission did not commit.
        PeripheralManager::getInstance().acceptCaptured=false;
        manager.dispatchPeriphExecEvent("exec_parent","success",origin);
        assert(manager.matched.size()==2);
        manager.dispatchPeriphExecEvent("exec_parent","success");
        assert(manager.matched.size()==3 && manager.origins.back().unbound());
    }
    {
        PeriphExecManager manager; auto rule=basicRule("stale-source");
        manager.rules[rule.id]=rule; // A current rule isolates the UART provenance failure.
        String reason; AsyncExecResult admitted;
        assert(!manager.dispatchAsync(rule,"captured",&reason,&admitted,FastBee::UartEventOrigin::captured(1,2)));
        assert(reason=="uart_source_unconfirmed" && manager.admissions==0 && manager.executions==0);
        assert(manager._runningRuleIds.empty() && admitted.operationId==0);
        assert(manager.workerDemandChecks==0 && manager.workerAdmissions==0);
    }
    {
        PeriphExecManager manager; auto rule=basicRule("worker-deferred");
        manager.rules[rule.id]=rule;manager.allowWorkers=false;
        String reason;AsyncExecResult admitted;
        assert(!manager.dispatchAsync(rule,"",&reason,&admitted));
        assert(reason=="worker_memory_pending" && manager.workerDemandChecks==1 && manager.workerAdmissions==1);
        assert(manager._runningRuleIds.empty() && manager.admissions==0 && manager.executions==0);
        assert(admitted.operationId==0);
    }
    {
        PeriphExecManager manager; auto rule=basicRule("local");
        assert(!manager.sanitizeRuleForSafety(rule) && rule.enabled);
        manager.rules[rule.id]=rule; assert(manager.enableRule(rule.id));
        String reason; AsyncExecResult admitted;
        assert(manager.runOnce(rule.id,"",&reason,&admitted));
        assert(manager.executions==2 && manager.admissions==1 && admitted.operationId==1);
        assert(admitted.ordinaryControl.request.actor.identity==rule.outputActor);
        assert(admitted.ordinaryControl.request.actor.source==FastBee::OrdinaryControl::Source::Rule);
        assert(admitted.ordinaryControl.ruleBound &&
            admitted.ordinaryControl.ruleEpoch==FastBee::RuleOutputIdentity::currentEpoch());
    }
    {
        PeriphExecManager manager;
        auto add=[&](const char* id,const ExecAction& output) {
            auto rule=basicRule(id); rule.actions={output};
            ExecTrigger trigger; trigger.triggerType=static_cast<uint8_t>(ExecTriggerType::EVENT_TRIGGER);
            trigger.eventId="mc:2"; rule.triggers.push_back(trigger); manager.rules[id]=rule;
        };
        add("same-direct",action(17,"modbus:2","1"));
        add("same-ctrl",action(10,"bus",R"({"ctrl":[{"d":2,"a":"forward"}]})"));
        add("other-direct",action(17,"modbus:3","1"));
        add("other-ctrl",action(10,"bus",R"({"ctrl":[{"d":3,"a":"forward"}]})"));
        add("local",action(0,"gpio"));
        manager.dispatchEventMatchedRules("mc:2",R"({"d":2,"a":"forward","origin":"motor-result"})");
        assert(manager.matched.size()==3);
        for (const auto& id:manager.matched) assert(!id.startsWith("same-"));
        manager.matched.clear();
        manager.dispatchEventMatchedRules("mc:2",R"({"d":2,"a":"forward","origin":"user"})");
        assert(manager.matched.size()==5); // Ordinary explicit command events remain valid.
    }
}
'''
        with tempfile.TemporaryDirectory(prefix="fastbee-retired-rules-") as temporary:
            path = Path(temporary); cpp = path / "rules.cpp"
            binary = path / ("rules.exe" if os.name == "nt" else "rules")
            arduino = (ROOT / "test/mocks/Arduino.h").read_text(encoding="utf-8")
            (path / "Arduino.h").write_text(arduino, encoding="utf-8")
            cpp.write_text(harness + production + checks, encoding="utf-8")
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-misleading-indentation", "-Wno-deprecated-declarations",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                f"-I{path}", f"-I{ROOT / 'include'}", f"-I{json_headers[0].parent}",
                str(cpp), str(ROOT / "src/network/LiteralStringMatch.cpp"), "-o", str(binary),
            ], capture_output=True, text=True, encoding="utf-8", errors="replace", env=environment, timeout=60)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", env=environment, timeout=15)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
