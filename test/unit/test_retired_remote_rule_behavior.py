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
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Existing ArduinoJson headers required")
        source = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
        production = "\n".join(method(source, signature) for signature in (
            "bool PeriphExecManager::sanitizeRuleForSafety(",
            "bool PeriphExecManager::getRuleSnapshot(",
            "bool PeriphExecManager::enableRule(",
            "bool PeriphExecManager::setRuleEnabledLocked(",
            "bool PeriphExecManager::runOnce(",
            "bool PeriphExecManager::dispatchAsync(",
            "void PeriphExecManager::dispatchEventMatchedRules(",
        ))
        harness = r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/PeripheralExecution.h"
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
struct MutexGuard { explicit MutexGuard(int) {} bool isLocked() const { return true; } };
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
enum class AsyncExecError { NONE,WORKER_QUEUE_FULL };
struct AsyncExecResult {
    unsigned operationId=0,queuedAt=0,deadlineAt=0,endTime=0;
    uint8_t queuePosition=0; bool asyncExecution=false;
    AsyncExecStatus status=AsyncExecStatus::PENDING;
    AsyncExecError errorCode=AsyncExecError::NONE;
};
struct PeriphExecManager;
struct AsyncExecContext {
    PeriphExecRule ruleCopy; String receivedValue; PeriphExecManager* manager=nullptr;
    void* mqtt=nullptr; int taskSlot=0;
    unsigned operationId=0,queuedAt=0,deadlineAt=0; uint8_t queuePosition=0;
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
    int executions=0,admissions=0,refreshes=0;
    std::vector<String> matched;
    bool ruleHasPollCollectionAction(const PeriphExecRule&) const { return false; }
    bool ruleNeedsModbus(const PeriphExecRule&) const { return false; }
    bool sanitizeTriggerForSafety(ExecTrigger&,bool,const String&) const { return false; }
    void refreshRuleRuntimeIndexesLocked() { ++refreshes; }
    bool sanitizeRuleForSafety(PeriphExecRule&) const;
    bool getRuleSnapshot(const String&,PeriphExecRule&) const;
    bool enableRule(const String&);
    bool setRuleEnabledLocked(const String&,bool,String&);
    bool runOnce(const String&,const String&,String*,AsyncExecResult*);
    bool dispatchAsync(const PeriphExecRule&,const String&,String*,AsyncExecResult*);
    void dispatchEventMatchedRules(const String&,const String&);
    bool shouldAvoidSyncFallback(const PeriphExecRule&) const { return false; }
    bool shouldRunAsync() const { return false; }
    bool beginOperation(const PeriphExecRule&,bool,uint8_t,AsyncExecResult& result) {
        result.operationId=++admissions; return true;
    }
    void executeSyncWithCompletion(const PeriphExecRule& rule,const String&,AsyncExecResult&) {
        executions+=rule.actions.size();
        _runningRuleIds.erase(rule.id); _runningStartTime.erase(rule.id);
    }
    void recordResult(const AsyncExecResult&) {}
    bool dispatchAndCommitTrigger(const String& id,size_t,const String&,const char*) {
        matched.push_back(id); return true;
    }
};
'''
        checks = r'''
ExecAction action(uint8_t type,const char* target="",const char* value="") {
    ExecAction result; result.actionType=type; result.targetPeriphId=target; result.actionValue=value; return result;
}
PeriphExecRule basicRule(const char* id) {
    PeriphExecRule rule; rule.id=id; rule.name=id; rule.enabled=true;
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
    assert(rule.actions.front().targetPeriphId=="gpio-first");
}
int main() {
    checkRetired(false); checkRetired(true);
    {
        PeriphExecManager manager; auto rule=basicRule("local");
        assert(!manager.sanitizeRuleForSafety(rule) && rule.enabled);
        manager.rules[rule.id]=rule; assert(manager.enableRule(rule.id));
        String reason; AsyncExecResult admitted;
        assert(manager.runOnce(rule.id,"",&reason,&admitted));
        assert(manager.executions==2 && manager.admissions==1 && admitted.operationId==1);
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
            arduino = arduino.replace("    bool equals(const String& other) const", """
    bool equalsIgnoreCase(const String& other) const {
        String left=*this, right=other; left.toLowerCase(); right.toLowerCase();
        return left==right;
    }
    bool equals(const String& other) const""")
            (path / "Arduino.h").write_text(arduino, encoding="utf-8")
            cpp.write_text(harness + production + checks, encoding="utf-8")
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            result = subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-misleading-indentation", "-Wno-deprecated-declarations",
                "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                f"-I{path}", f"-I{ROOT / 'include'}", f"-I{json_headers[0].parent}",
                str(cpp), "-o", str(binary),
            ], capture_output=True, text=True, encoding="utf-8", errors="replace", env=environment, timeout=60)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", env=environment, timeout=15)
            self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
