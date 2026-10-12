"""Prepared real source validator/action guard and queue provenance contracts."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_retired_remote_rule_behavior import method

ROOT = Path(__file__).resolve().parents[2]


class UartEventOriginTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        uart = (ROOT/'src/core/PeripheralManagerUART.cpp').read_text(encoding='utf-8')
        executor = (ROOT/'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        cls.execute = method(executor, 'std::vector<ActionExecResult> PeriphExecExecutor::executeAllActions(')
        validator = method(uart, 'bool PeripheralManager::isUartEventOriginCurrent(')
        guard = method(cls.execute, 'auto sourceCurrent =') + ';'
        harness = r'''
#include "core/UartEventOrigin.h"
#include "core/UartReceiveGeneration.h"
#include "core/UartReceiveFault.h"
#include "core/UartPortLease.h"
#include "core/SensorConfigEpoch.h"
#include <cassert>
#include <map>
#include <string>
#include <type_traits>
static bool busy = false;
struct RecursiveMutexGuard {
    RecursiveMutexGuard(int,int) {}
    bool isLocked() const { return !busy; }
};
struct PeripheralManager {
    int _mutex = 1;
    std::map<int,uint8_t> uartPortById;
    FastBee::UartPortLease uartPortLeases[3];
    FastBee::UartReceiveGeneration uartReceiveGenerations;
    static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const;
};
enum class AsyncExecError { NONE, UART_SOURCE_UNCONFIRMED, RULE_REVISION_UNCONFIRMED };
struct PeriphExecRule {};
struct RuleManager { bool isRuleExecutionCurrent(const PeriphExecRule&) const { return true; } } manager;
struct ExecAction {};
struct AsyncExecResult { FastBee::UartEventOrigin uartOrigin; AsyncExecError errorCode = AsyncExecError::NONE; };
unsigned traces = 0;
void appendActionTrace(AsyncExecResult*,size_t,const ExecAction&,const char*,bool ok,int,int,const char* reason) {
    assert(!ok && std::string(reason) == "uart_source_unconfirmed"); ++traces;
}
// @validator@
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode = argv[1];
    static_assert(std::is_trivially_copyable<FastBee::UartEventOrigin>::value, "No queued source allocation");
    auto& pm = PeripheralManager::getInstance();
    assert(pm.uartPortLeases[1].acquire(1));
    assert(pm.uartReceiveGenerations.begin(1)); pm.uartPortById[11] = 1;
    FastBee::UartReceiveFault::arm(1,pm.uartReceiveGenerations.current(1));
    AsyncExecResult trace;
    trace.uartOrigin = FastBee::UartEventOrigin::captured(FastBee::SensorConfigEpoch::current(),
        pm.uartReceiveGenerations.current(1));
    const auto queuedCopy = trace.uartOrigin;
    AsyncExecResult* traceOut = &trace;
    bool completed = true; bool* completedAllOut = &completed;
    ExecAction action;
    PeriphExecRule rule; auto* _manager = &manager;
    // @guard@
    assert(sourceCurrent(0, action)); // Initial admission / preceding action.
    if (mode == "rebegin") assert(pm.uartReceiveGenerations.begin(1));
    else if (mode == "hardware_fault") FastBee::UartReceiveFault::observe(1,trace.uartOrigin.receiveGeneration,FastBee::UartReceiveFault::Error::Parity);
    else if (mode == "config_change") FastBee::SensorConfigEpoch::advance();
    else if (mode == "stop") pm.uartReceiveGenerations.end(1);
    else if (mode == "lost_owner") assert(pm.uartPortLeases[1].releaseAfterStop());
    else if (mode == "map_removed") pm.uartPortById.clear();
    else if (mode == "busy") busy = true;
    else if (mode == "unknown") trace.uartOrigin = FastBee::UartEventOrigin::captured(0,0);
    else if (mode == "malformed") trace.uartOrigin.required = false;
    else if (mode == "unbound") { trace.uartOrigin = {}; busy = true; }
    else assert(mode == "same");
    const bool expected = mode == "same" || mode == "unbound";
    assert(sourceCurrent(1, action) == expected); // Post-wait / next-action check.
    assert(completed == expected && traces == (expected ? 0U : 1U));
    assert((trace.errorCode == AsyncExecError::UART_SOURCE_UNCONFIRMED) == !expected);
    assert(queuedCopy.required && queuedCopy.configEpoch != 0 && queuedCopy.receiveGeneration != 0);
    assert(queuedCopy.receiveGeneration != pm.uartReceiveGenerations.current(1) || mode != "rebegin");
}
'''.replace('// @validator@', validator).replace('// @guard@', guard)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-uart-origin-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name)/'origin.cpp'
        source.write_text(harness, encoding='utf-8')
        cls.binary = Path(cls.temp.name)/'origin.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), str(source), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_production_preserves_source_through_all_dispatch_paths(self):
        manager = (ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        manager += '\n' + (ROOT / 'src/core/PeriphExecWorkers.cpp').read_text(encoding='utf-8')
        scheduler = (ROOT/'src/core/PeriphExecScheduler.cpp').read_text(encoding='utf-8')
        frame = method(scheduler, 'void PeriphExecScheduler::checkSerialEvents(')
        self.assertIn('UartEventOrigin::captured(epoch, generation)', frame)
        self.assertIn('handlePollData("serial", _serializeBuffer, uartOrigin)', frame)
        for text, signature, call in (
            (manager, 'void PeriphExecManager::handlePollData(', 'handlePollData(source, data, uartOrigin)'),
            (scheduler, 'void PeriphExecScheduler::handlePollData(', 'handleDataEvent(source, data, DataSourceType::POLL, uartOrigin)'),
            (scheduler, 'void PeriphExecScheduler::handleDataEvent(', 'processPollDataMatch(arr, source, uartOrigin)'),
            (manager, 'void PeriphExecManager::processPollDataMatch(', 'matched.receivedValue, "poll-data", uartOrigin, matched.revision)'),
            (manager, 'bool PeriphExecManager::dispatchAndCommitTrigger(', 'dispatchByRuleId(ruleId, receivedValue, &rejectionReason, uartOrigin, expectedRevision, outputSource)'),
            (manager, 'bool PeriphExecManager::dispatchByRuleId(', 'dispatchAsync(ruleCopy, receivedValue, rejectionReason, nullptr, uartOrigin, nullptr, automaticSource)'),
        ):
            self.assertIn(call, method(text, signature))
        admission = method(manager, 'bool PeriphExecManager::dispatchAsync(')
        self.assertIn('beginOperation(rule, false, 0, operation, uartOrigin, capturedControl)', admission)
        self.assertIn('beginOperation(rule, true, queuePosition, operation, uartOrigin, capturedControl)', admission)
        self.assertLess(admission.index('ctx->uartOrigin = operation.uartOrigin'), admission.index('_workerPool->enqueue(ctx)'))
        self.assertLess(admission.index('ctx->ordinaryControl = operation.ordinaryControl'), admission.index('_workerPool->enqueue(ctx)'))
        for signature in ('void PeriphExecManager::executeWorkerJob(', 'void PeriphExecManager::cancelWorkerJob('):
            self.assertIn('result.uartOrigin = ctx->uartOrigin', method(manager, signature))
        self.assertIn('operation.deadlineAt, &operation', method(manager, 'bool PeriphExecManager::executeSyncWithCompletion('))

    def test_guard_runs_before_and_after_delay_and_error_is_exposed(self):
        first = self.execute.index('if (!sourceCurrent(actionIndex, action)) break;')
        delay = self.execute.index('delay(delayMs);')
        last = self.execute.index('if (!sourceCurrent(actionIndex, action)) break;', first + 1)
        self.assertLess(first, delay)
        self.assertLess(delay, last)
        self.assertLess(last, self.execute.index('executeSensorReadGroup('))
        for name in ('src/network/handlers/PeriphExecRouteHandler.cpp', 'src/core/ScreenPanelManager.cpp'):
            self.assertIn('case AsyncExecError::UART_SOURCE_UNCONFIRMED: return "uart_source_unconfirmed";',
                          (ROOT/name).read_text(encoding='utf-8'))


for _case in ('same', 'rebegin', 'hardware_fault', 'config_change', 'stop', 'lost_owner', 'map_removed',
              'busy', 'unknown', 'malformed', 'unbound'):
    setattr(UartEventOriginTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
