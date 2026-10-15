"""Real planner workspace allocation and route locks with deterministic OOM."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_peripheral_route_fixed_json import definition
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class PreflightWorkspaceLifecycleTests(unittest.TestCase):
    def test_requests_release_large_workspace_and_reject_oom_or_concurrent_use(self):
        route_path = ROOT/'src/network/handlers/PeripheralRouteHandler.cpp'
        guard_path = ROOT/'include/core/AsyncExecTypes.h'
        inputs = [route_path, guard_path, ROOT/'include/core/PeripheralResourcePlanner.h']
        hashes = [hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs]
        route = route_path.read_text(encoding='utf8')
        guard = guard_path.read_text(encoding='utf8')
        method = definition(route, 'PeripheralRouteHandler::handlePreflightPeripheral')
        lock_start = method.index('    MutexGuard resultLock(')
        lock_end = method.index('    PeripheralConfig candidate;', lock_start)
        workspace_start = method.index('    std::unique_ptr<FastBee::PreflightResult> workspace;')
        workspace_end = method.index('    const std::vector<PeripheralConfig> existing', workspace_start)
        admission = 'void handle(AsyncWebServerRequest* request) {\n' + method[lock_start:lock_end] + method[workspace_start:workspace_end] + '''
    assert(preflightResult.allowed && preflightResult.claimCount==0 && preflightResult.issueCount==0);
    if (request->validationFails) return;
    if (request->interrupts) throw 17;
    ++writes;
}\n'''
        program = r'''
#include <Arduino.h>
#undef malloc
#undef free
#include "core/PeripheralResourcePlanner.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <cstring>
struct Semaphore { std::recursive_timed_mutex mutex; };
using SemaphoreHandle_t=Semaphore*; using TickType_t=unsigned;
constexpr unsigned portMAX_DELAY=1000; constexpr int pdTRUE=1;
#define pdMS_TO_TICKS(value) (value)
int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned ticks) {
    return semaphore->mutex.try_lock_for(std::chrono::milliseconds(ticks)) ? pdTRUE : 0;
}
void xSemaphoreGive(SemaphoreHandle_t semaphore) { semaphore->mutex.unlock(); }
int xSemaphoreTakeRecursive(SemaphoreHandle_t semaphore, unsigned ticks) { return xSemaphoreTake(semaphore,ticks); }
void xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore) { xSemaphoreGive(semaphore); }
// @guards@
static std::atomic<bool> oom{false};
static std::atomic<unsigned> allocations{0}, live{0};
static std::atomic<void*> owned{nullptr};
void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    assert(bytes==sizeof(FastBee::PreflightResult));
    if (oom) return nullptr;
    void* pointer=std::malloc(bytes);
    if (pointer) { assert(live.fetch_add(1)==0); owned=pointer; ++allocations; }
    return pointer;
}
void operator delete(void* pointer) noexcept {
    if (pointer && pointer==owned.load()) { owned=nullptr; assert(live.fetch_sub(1)==1); }
    std::free(pointer);
}
void operator delete(void* pointer,size_t) noexcept { ::operator delete(pointer); }
struct AsyncWebServerRequest { int error=0; bool validationFails=false,interrupts=false; String message; };
struct WebHandlerContext {
    void sendError(AsyncWebServerRequest* request,int code,const char* text) {request->error=code;request->message=text;}
};
struct PeripheralManager {
    Semaphore mutex;
    static PeripheralManager& getInstance(){static PeripheralManager manager;return manager;}
    SemaphoreHandle_t getMutex(){return &mutex;}
};
// The extracted route body constructs the concrete form adapter; provide a
// fieldless standalone stub so the workspace/lock lifecycle compiles without
// pulling the real adapter's interface header (and its ArduinoJson chain).
class WebPeripheralFormFields {
public:
    WebPeripheralFormFields(WebHandlerContext*, AsyncWebServerRequest*) {}
    bool has(const char*) const { return false; }
    String value(const char*, const char* fallback) const { return fallback; }
    int intValue(const char*, int fallback) const { return fallback; }
    bool boolValue(const char*, bool fallback) const { return fallback; }
};
// @factory@
struct RequestHandler {
    WebHandlerContext* ctx;
    Semaphore planner;
    SemaphoreHandle_t preflightMutex=&planner;
    unsigned writes=0;
    explicit RequestHandler(WebHandlerContext* context):ctx(context) {}
    // @admission@
};
int main() {
    WebHandlerContext context;RequestHandler handler{&context};
    assert(live==0 && allocations==0); // no workspace while idle
    for (unsigned i=0;i<200;++i) {
        AsyncWebServerRequest request;handler.handle(&request);
        assert(request.error==0 && live==0 && handler.writes==i+1);
    }
    const unsigned before=handler.writes;
    {AsyncWebServerRequest request;request.validationFails=true;handler.handle(&request);
     assert(live==0 && handler.writes==before);}
    {AsyncWebServerRequest request;request.interrupts=true;
     try {handler.handle(&request);assert(false);}catch(int code){assert(code==17);}
     assert(live==0 && handler.writes==before);}
    oom=true;
    {AsyncWebServerRequest request;handler.handle(&request);
     assert(request.error==503 && std::strstr(request.message.c_str(),"PREFLIGHT_MEMORY_UNAVAILABLE:"));
     assert(live==0 && handler.writes==before);}
    oom=false;
    const unsigned beforeBusy=allocations;
    handler.planner.mutex.lock();
    {AsyncWebServerRequest competing;
     std::thread other([&]{handler.handle(&competing);});other.join();
     assert(competing.error==503 && handler.writes==before && allocations==beforeBusy && live==0);}
    handler.planner.mutex.unlock();
    auto& manager=PeripheralManager::getInstance();manager.mutex.mutex.lock();
    {AsyncWebServerRequest competing;
     std::thread other([&]{handler.handle(&competing);});other.join();
     assert(competing.error==503 && handler.writes==before && allocations==beforeBusy && live==0);}
    manager.mutex.mutex.unlock();
    {AsyncWebServerRequest request;handler.handle(&request);
     assert(request.error==0 && handler.writes==before+1 && live==0);}
    std::cout<<"preflight workspace: real result "<<sizeof(FastBee::PreflightResult)
             <<" host bytes; 200 repeated, validation/exception cleanup, OOM, planner/manager contention and recovery passed\n";
}
'''
        program = program.replace('// @factory@', definition(route,'allocatePreflightWorkspace'))
        program = program.replace('// @guards@', cpp_function(guard,'class MutexGuard {')+';\n'+cpp_function(guard,'class RecursiveMutexGuard {')+';')
        program = program.replace('// @admission@', admission)
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler,'C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-preflight-workspace-') as directory:
            temp=Path(directory);cpp=temp/'workspace.cpp';cpp.write_text(program,encoding='utf8')
            binary=temp/('workspace.exe' if os.name=='nt' else 'workspace')
            built=subprocess.run([compiler,'-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'test/mocks'),'-I'+str(ROOT/'include'),str(cpp),'-o',str(binary)],
                capture_output=True,text=True,timeout=90)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            print(result.stdout.strip())
        self.assertEqual(hashes,[hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs])


if __name__=='__main__':unittest.main()
