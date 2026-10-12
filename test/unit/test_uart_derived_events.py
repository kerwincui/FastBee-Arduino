"""Prepared production event-router provenance behavior; no hardware involved."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_retired_remote_rule_behavior import method

ROOT = Path(__file__).resolve().parents[2]


class UartDerivedEventTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        manager = (ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        manager += '\n' + (ROOT / 'src/core/PeriphExecWorkers.cpp').read_text(encoding='utf-8')
        scheduler = (ROOT/'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        production = '\n'.join(method(text, signature) for text, signature in (
            (manager, 'void PeriphExecManager::triggerEvent('),
            (manager, 'void PeriphExecManager::triggerEventById('),
            (manager, 'bool PeriphExecManager::notifyMqttEventPublish('),
            (scheduler, 'void PeriphExecScheduler::triggerEvent('),
            (scheduler, 'void PeriphExecScheduler::triggerEventById('),
            (scheduler, 'void PeriphExecScheduler::triggerPeriphExecEvent('),
        ))
        harness = r'''
#include <Arduino.h>
#include "core/UartEventOrigin.h"
#include <cassert>
#include <functional>
#include <vector>
enum class EventType { START };
struct EventDef { const char* id; const char* name; EventType type; };
const EventDef STATIC_EVENTS[]={{"start","Start",EventType::START},{nullptr,nullptr,EventType::START}};
const EventDef* findStaticEvent(const char* id) { return String(id)=="start" ? &STATIC_EVENTS[0] : nullptr; }
enum class PeripheralType { DEVICE_EVENT };
struct PeripheralConfig { PeripheralType type=PeripheralType::DEVICE_EVENT; String name="Event"; };
struct PeripheralManager {
    bool current=true;
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
        return origin.unbound() || (current && origin.matches(7,9));
    }
    bool getPeripheralSnapshot(const String& id,PeripheralConfig&) { return id=="device"; }
};
struct Logger { template<class... T> void infof(T...) {} template<class... T> void warningf(T...) {} } LOGGER;
struct PeriphExecScheduler;
struct PeriphExecManager {
    PeriphExecScheduler* _scheduler=nullptr;
    bool initialized=true,retireInChild=false;
    unsigned published=0;
    std::vector<FastBee::UartEventOrigin> origins;
    std::function<bool(const String&,const String&,const String&)> _mqttEventPublishCb;
    bool isInitialized() const { return initialized; }
    void triggerEvent(EventType,const String&,const FastBee::UartEventOrigin& = {});
    void triggerEventById(const String&,const String&,const FastBee::UartEventOrigin& = {},bool = true);
    bool notifyMqttEventPublish(const String&,const String&,const String&,const FastBee::UartEventOrigin& = {});
    void dispatchEventMatchedRules(const String&,const String&,const FastBee::UartEventOrigin& origin) {
        assert(PeripheralManager::getInstance().isUartEventOriginCurrent(origin));
        origins.push_back(origin);
        if(retireInChild) PeripheralManager::getInstance().current=false;
    }
    void dispatchPeriphExecEvent(const String& id,const String& data,const FastBee::UartEventOrigin& origin) {
        dispatchEventMatchedRules(id,data,origin);
    }
};
struct PeriphExecScheduler {
    PeriphExecManager* _manager;
    void triggerEvent(EventType,const String&,const FastBee::UartEventOrigin&,bool = true);
    void triggerEventById(const String&,const String&,const FastBee::UartEventOrigin&,bool = true);
    void triggerPeriphExecEvent(const String&,const String&,const FastBee::UartEventOrigin&);
};
// @production@
int main(int argc,char** argv) {
    assert(argc==2); const String mode=argv[1];
    PeriphExecManager manager; PeriphExecScheduler scheduler{&manager}; manager._scheduler=&scheduler;
    manager._mqttEventPublishCb=[&](const String&,const String&,const String& value){
        assert(value=="payload"); ++manager.published; return true;
    };
    auto origin=FastBee::UartEventOrigin::captured(7,9);
    String id="device";
    if(mode=="static") id="start";
    if(mode=="exec") id="exec_parent";
    if(mode=="modbus") id="mc:2";
    if(mode=="unknown") id="unknown";
    if(mode=="stale") PeripheralManager::getInstance().current=false;
    if(mode=="malformed") origin.required=false;
    if(mode=="unbound") {origin={};PeripheralManager::getInstance().current=false;}
    if(mode=="child_retires") manager.retireInChild=true;
    if(mode=="uninitialized") manager.initialized=false;
    if(mode=="no_scheduler") manager._scheduler=nullptr;
    if(mode=="type") manager.triggerEvent(EventType::START,"payload",origin);
    else manager.triggerEventById(id,"payload",origin);
    const bool rejected=mode=="stale" || mode=="malformed" || mode=="uninitialized" || mode=="no_scheduler";
    assert(manager.origins.size()==((rejected || mode=="unknown") ? 0U : 1U));
    const bool publish=!rejected && mode!="exec" && mode!="modbus" && mode!="child_retires";
    assert(manager.published==(publish ? 1U : 0U));
    for(const auto& recorded:manager.origins) {
        assert(recorded.configEpoch==origin.configEpoch && recorded.receiveGeneration==origin.receiveGeneration);
        assert(recorded.required==origin.required);
    }
}
'''.replace('// @production@', production)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-derived-origin-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name)/'derived.cpp'
        source.write_text(harness, encoding='utf-8')
        cls.binary = Path(cls.temp.name)/'derived.exe'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_completion_and_action_call_sites_preserve_origin(self):
        manager = (ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        manager += '\n' + (ROOT / 'src/core/PeriphExecWorkers.cpp').read_text(encoding='utf-8')
        for signature, call in (
            ('void PeriphExecManager::cancelWorkerJob(', '"cancelled", ctx->uartOrigin)'),
            ('void PeriphExecManager::executeWorkerJob(', '"success" : "failed", ctx->uartOrigin)'),
            ('bool PeriphExecManager::executeSyncWithCompletion(', '"success" : "failed", operation.uartOrigin)'),
        ):
            self.assertIn(call, method(manager, signature))
        for signature, label in (('dispatchEventMatchedRules', 'event'), ('dispatchPeriphExecEvent', 'periph-event')):
            body = method(manager, 'void PeriphExecManager::'+signature+'(')
            self.assertLess(body.index('isUartEventOriginCurrent(uartOrigin)'), body.index('MutexGuard lock(_rulesMutex)'))
            self.assertIn('eventData, "'+label+'", uartOrigin, matched.revision)', body)
        executor = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        body = method(executor, 'std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(')
        self.assertIn('eventOrigin = traceOut ? traceOut->uartOrigin : FastBee::UartEventOrigin{}', body)
        self.assertIn('triggerEventById(triggerEventId, effectiveValue, eventOrigin, false)', body)
        self.assertIn('triggerEventId, rule.name, effectiveValue, eventOrigin)', body)

    def test_system_restart_notification_and_composite_rejection_keep_origin(self):
        executor = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        system = method(executor, 'bool PeriphExecExecutor::executeSystemAction(')
        self.assertEqual(system.count('triggerEvent(EventType::EVENT_SYS_RESTART, "", uartOrigin)'), 2)
        self.assertIn('executeSystemAction(action, uartOrigin, sourceUnconfirmed)',
                      method(executor, 'bool PeriphExecExecutor::executeActionItem('))
        run = method(executor, 'std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(')
        stopped = method(run, 'if (sourceUnconfirmed)')
        self.assertIn('AsyncExecError::UART_SOURCE_UNCONFIRMED', stopped)
        self.assertIn('*completedAllOut = false', stopped)
        self.assertIn('break;', stopped)


for _case in ('device', 'static', 'type', 'exec', 'modbus', 'unknown', 'stale', 'malformed',
              'unbound', 'child_retires', 'uninitialized', 'no_scheduler'):
    setattr(UartDerivedEventTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
