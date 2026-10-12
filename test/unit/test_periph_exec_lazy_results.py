"""Execute production lazy result storage and admission under allocation failure."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class PeriphExecLazyResultsTests(unittest.TestCase):
    def test_real_storage_and_manager_admission_recover_without_losing_active_results(self):
        manager = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/core/PeriphExecManager.h").read_text(encoding="utf-8")
        types = (ROOT / "include/core/AsyncExecTypes.h").read_text(encoding="utf-8")
        production = []
        for marker in ("enum class AsyncExecStatus : uint8_t", "enum class AsyncExecError : uint8_t",
                       "struct AsyncExecActionTrace", "struct AsyncExecResult"):
            production.append(marker + " {" + braced_block(types, marker) + "};")
        production.append("class ExecutionResultHistory {" +
                          braced_block(header, "class ExecutionResultHistory") + "};")
        production.append("static bool isTerminalExecStatus(AsyncExecStatus status) {" +
                          braced_block(manager, "static bool isTerminalExecStatus") + "}")
        prefix = r'''
#include <cassert>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include "core/UartEventOrigin.h"
#include "core/NativeOutputControl.h"
#include "core/SensorConfigEpoch.h"
struct PeripheralManager {
 static PeripheralManager& getInstance() { static PeripheralManager value; return value; }
 bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
  return origin.unbound() || origin.matches(FastBee::SensorConfigEpoch::current(), 71);
 }
};
using String=std::string;
constexpr size_t ASYNC_EXEC_ACTION_TRACE_LIMIT=4;
constexpr size_t ASYNC_EXEC_RESULT_HISTORY_LIMIT=6;
constexpr unsigned long ASYNC_EXEC_OPERATION_TIMEOUT_MS=30000;
static bool failArray=false, lockAllowed=true;
static size_t arrayAttempts=0;
static size_t free8=50000, largest8=50000, afterAllocationFree8=50000;
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
 ++arrayAttempts;
 if (failArray) return nullptr;
 free8=afterAllocationFree8;
 try { return ::operator new[](bytes); } catch (...) { return nullptr; }
}
namespace FastBee { namespace MemoryBudget {
static size_t available=50000;
size_t dramFree(){return available;}
size_t dram8Free(){return free8;}
size_t dram8Largest(){return largest8;}
}}
unsigned long millis(){return 1234;}
struct MutexGuard { explicit MutexGuard(int){} bool isLocked() const{return lockAllowed;} };
struct PeriphExecRule {String id="rule", name="Rule"; std::vector<int> actions={1,2};};
'''
        declaration = r'''
class PeriphExecManager {
public:
 int _resultsMutex=1;
 ExecutionResultHistory executionResults;
 uint32_t _nextOperationId=0;
 bool ensureResultHistoryCapacity();
 bool recordResult(const AsyncExecResult& result);
 bool beginOperation(const PeriphExecRule& rule, bool asyncExecution,
                     uint8_t queuePosition, AsyncExecResult& operation,
                     const FastBee::UartEventOrigin& uartOrigin = {},
                     const FastBee::OrdinaryControlContext& control = {});
 std::vector<AsyncExecResult> getRecentResults();
};
'''
        for signature in (
            "bool PeriphExecManager::ensureResultHistoryCapacity()",
            "bool PeriphExecManager::recordResult(const AsyncExecResult& result)",
            "bool PeriphExecManager::beginOperation(const PeriphExecRule& rule, bool asyncExecution, uint8_t queuePosition, AsyncExecResult& operation, const FastBee::UartEventOrigin& uartOrigin, const FastBee::OrdinaryControlContext& control)",
            "std::vector<AsyncExecResult> PeriphExecManager::getRecentResults()",
        ):
            marker = signature.split("(")[0]
            declaration += signature + " {" + braced_block(manager, marker) + "}\n"
        checks = r'''
int main(){
 PeriphExecManager m;
 PeriphExecRule rule;
 AsyncExecResult admitted;
 assert(arrayAttempts==0 && m.getRecentResults().empty());
 const auto stale=FastBee::UartEventOrigin::captured(FastBee::SensorConfigEpoch::current(),70);
 assert(!m.beginOperation(rule,true,1,admitted,stale));
 assert(admitted.errorCode==AsyncExecError::UART_SOURCE_UNCONFIRMED && arrayAttempts==0);
 assert(m._nextOperationId==0 && m.getRecentResults().empty());
 FastBee::MemoryBudget::available=11999; free8=11999;
 assert(!m.beginOperation(rule,true,1,admitted));
 assert(arrayAttempts==0 && admitted.operationId==0 && m._nextOperationId==0);
 FastBee::MemoryBudget::available=50000;
 const size_t requiredBytes=sizeof(AsyncExecResult)*ASYNC_EXEC_RESULT_HISTORY_LIMIT;
 free8=12000+requiredBytes-1;
 assert(!m.beginOperation(rule,true,1,admitted));
 assert(arrayAttempts==0 && admitted.operationId==0);
 free8=50000; largest8=requiredBytes-1;
 assert(!m.beginOperation(rule,true,1,admitted));
 assert(arrayAttempts==0 && m._nextOperationId==0);
 largest8=50000; afterAllocationFree8=11999;
 assert(!m.beginOperation(rule,true,1,admitted));
 assert(!m.executionResults.hasCapacity() && m.executionResults.size()==0);
 assert(admitted.operationId==0 && m._nextOperationId==0);
 free8=50000; afterAllocationFree8=50000;
 failArray=true;
 for(int i=0;i<3;++i) {
  assert(!m.beginOperation(rule,true,1,admitted));
  assert(m.executionResults.size()==0 && admitted.operationId==0 && m._nextOperationId==0);
 }
 failArray=false;
 assert(m.beginOperation(rule,true,1,admitted));
 const size_t attemptsAfterAdmission=arrayAttempts;
 assert(admitted.operationId==1 && admitted.status==AsyncExecStatus::PENDING);
 assert(m.getRecentResults()[0].ruleName=="Rule");
 for(int i=1;i<6;++i) assert(m.beginOperation(rule,true,1,admitted));
 assert(!m.beginOperation(rule,true,1,admitted));
 assert(m.executionResults.size()==6 && m._nextOperationId==6);
 AsyncExecResult terminal=m.getRecentResults()[0];
 terminal.status=AsyncExecStatus::COMPLETED;
 terminal.actionTraceCount=1;
 terminal.actionTraces[0].success=true;
 FastBee::MemoryBudget::available=0; free8=0; largest8=0; failArray=true;
 assert(m.recordResult(terminal));
 assert(m.getRecentResults()[0].actionTraces[0].success);
 assert(arrayAttempts==attemptsAfterAdmission);
 FastBee::MemoryBudget::available=50000;
 // Capacity already exists, but wider internal RAM cannot pay for new Strings.
 // Existing completion updates must still work; new admission must not evict
 // the completed result or consume an operation id until byte RAM recovers.
 free8=11999; largest8=11999;
 const auto beforePressure=m.getRecentResults();
 if(m.beginOperation(rule,false,0,admitted)) return 86;
 assert(admitted.operationId==0 && m._nextOperationId==6);
 AsyncExecResult newResult=terminal; newResult.operationId=999;
 if(m.recordResult(newResult)) return 86;
 assert(m.getRecentResults().size()==beforePressure.size());
 assert(m.getRecentResults()[0].operationId==terminal.operationId);
 assert(m.getRecentResults()[0].status==AsyncExecStatus::COMPLETED);
 assert(arrayAttempts==attemptsAfterAdmission);
 free8=12000; largest8=12000;
 assert(m.beginOperation(rule,false,0,admitted));
 assert(admitted.operationId==7 && !admitted.asyncExecution);
 assert(m.getRecentResults()[0].operationId==2);
 for(int i=0;i<100;++i) {
  terminal=m.getRecentResults().back(); terminal.status=AsyncExecStatus::FAILED;
  assert(m.recordResult(terminal));
  assert(m.beginOperation(rule,true,1,admitted));
  assert(m.executionResults.size()==6);
 }
 assert(arrayAttempts==attemptsAfterAdmission);
 for(int i=0;i<5;++i) assert(m.getRecentResults()[i].operationId==static_cast<uint32_t>(i+2));
 PeriphExecManager untracked;
 free8=50000; largest8=50000;
 terminal.operationId=42;
 assert(!untracked.recordResult(terminal));
 assert(untracked.getRecentResults().empty());
 failArray=false;
 assert(untracked.recordResult(terminal));
 assert(untracked.getRecentResults()[0].operationId==42);
 lockAllowed=false;
 const size_t previousAttempts=arrayAttempts;
 PeriphExecManager busy;
 assert(!busy.beginOperation(rule,true,1,admitted));
 assert(!busy.recordResult(terminal));
 assert(arrayAttempts==previousAttempts && busy.executionResults.size()==0);
 lockAllowed=true;
 PeriphExecManager sourceBound;
 const auto origin=FastBee::UartEventOrigin::captured(FastBee::SensorConfigEpoch::current(),71);
 FastBee::OrdinaryControlContext control;
 control.request.actor.source=FastBee::OrdinaryControl::Source::Rule;
 control.request.actor.identity=99; control.request.operation=123;
 control.configEpoch=origin.configEpoch; control.uartOrigin=origin;
 assert(sourceBound.beginOperation(rule,true,1,admitted,origin,control));
 assert(admitted.ordinaryControl.request.actor.identity==99);
 assert(admitted.ordinaryControl.request.operation==123);
 assert(admitted.ordinaryControl.configEpoch==origin.configEpoch);
 assert(admitted.uartOrigin.matches(origin.configEpoch,71));
 assert(admitted.peripheralConfigEpoch==origin.configEpoch);
 const auto history=sourceBound.getRecentResults();
 FastBee::SensorConfigEpoch::advance();
 assert(!sourceBound.beginOperation(rule,false,0,admitted,origin));
 assert(sourceBound.getRecentResults().size()==history.size() && sourceBound._nextOperationId==1);
 assert(sourceBound.getRecentResults()[0].uartOrigin.configEpoch==origin.configEpoch);
}
'''
        compiler = shutil.which("g++") or "D:/msys64/ucrt64/bin/g++.exe"
        with tempfile.TemporaryDirectory(prefix="fastbee-lazy-results-") as directory:
            source = Path(directory) / "test.cpp"
            source.write_text(prefix + "\n".join(production) + declaration + checks, encoding="utf-8")
            executable = Path(directory) / "test.exe"
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    '-I'+str(ROOT/'include'),
                                    str(source), "-o", str(executable)], capture_output=True,
                                   text=True, timeout=60, env=environment)
            self.assertEqual(0, build.returncode, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True,
                                 timeout=10, env=environment)
            self.assertEqual(0, run.returncode, run.stdout + run.stderr)
            # Each previous wide-pool guard independently admits work with
            # insufficient byte-addressable RAM once history capacity exists.
            for signature in ("bool PeriphExecManager::beginOperation",
                              "bool PeriphExecManager::recordResult"):
                body = braced_block(declaration, signature)
                old_body = body.replace("MemoryBudget::dram8Free() < 12000",
                                        "MemoryBudget::dramFree() < 12000")
                self.assertNotEqual(body, old_body)
                mutant = declaration.replace(body, old_body, 1)
                source.write_text(prefix + "\n".join(production) + mutant + checks,
                                  encoding="utf-8")
                build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                        '-I'+str(ROOT/'include'),
                                        str(source), "-o", str(executable)], capture_output=True,
                                       text=True, timeout=60, env=environment)
                self.assertEqual(0, build.returncode, build.stdout + build.stderr)
                run = subprocess.run([str(executable)], capture_output=True, text=True,
                                     timeout=10, env=environment)
                self.assertEqual(86, run.returncode, signature + run.stdout + run.stderr)

    def test_startup_and_worker_pause_do_not_allocate_or_discard_result_history(self):
        source = (ROOT / "src/core/PeriphExecManager.cpp").read_text(encoding="utf-8")
        source += '\n' + (ROOT / 'src/core/PeriphExecWorkers.cpp').read_text(encoding="utf-8")
        initialize = braced_block(source, "bool PeriphExecManager::initialize()")
        self.assertNotIn("executionResults.reserve", initialize)
        self.assertNotIn("executionResults.ensureCapacity", initialize)
        for name in ("suspendWorkersForMemory", "resumeWorkersAfterMemory"):
            body = braced_block(source, "bool PeriphExecManager::" + name + "()")
            self.assertNotIn("executionResults", body)


if __name__ == "__main__":
    unittest.main()
