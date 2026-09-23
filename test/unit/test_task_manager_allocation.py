"""Run production scheduler/CRUD with heap allocations denied during dispatch."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class TaskManagerAllocationTests(unittest.TestCase):
    def test_bounded_snapshot_survives_oom_mutations_and_budget_deferral(self):
        source = (ROOT / "src/systems/TaskManager.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/systems/TaskManager.h").read_text(encoding="utf-8")
        definitions = header[header.index("enum class TaskPriority"):header.index("class TaskManager")]
        fields = header[header.index("private:", header.index("class TaskManager")) + len("private:"):header.rindex("};")]
        signatures = [
            "void TaskManager::run()",
            "bool TaskManager::removeTask(const String& name)",
            "bool TaskManager::disableTask(const String& name)",
            "void TaskManager::clearAllTasks()",
        ]
        methods = "\n".join(signature + "{" + braced_block(source, signature) + "}"
                            for signature in signatures)
        add_begin = source.index("bool TaskManager::addTask", source.index("bool TaskManager::addTask") + 1)
        add_signature = source[add_begin:source.index("{", add_begin)]
        methods += add_signature + "{" + braced_block(source, add_signature) + "}"
        cpp = r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>
using String=std::string;
using std::min;using std::max;
using TaskFunction=void(*)(void*);
bool denyAllocations=false;unsigned deniedAllocations=0;
void* operator new(std::size_t n){if(denyAllocations){++deniedAllocations;throw std::bad_alloc();}
 if(void* p=std::malloc(n)){return p;}throw std::bad_alloc();}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
namespace TaskScheduler{constexpr uint8_t MAX_TASKS=20;}
unsigned long nowMs=1000;unsigned long millis(){return nowMs;}
int mutexDepth=0;using SemaphoreHandle_t=void*;
constexpr int portMAX_DELAY=-1;
void xSemaphoreTake(SemaphoreHandle_t,int){assert(mutexDepth==0);++mutexDepth;}
void xSemaphoreGive(SemaphoreHandle_t){assert(mutexDepth==1);--mutexDepth;}
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
#define FASTBEE_ENABLE_HEALTH_MONITOR 1
enum class MemoryGuardLevel{NORMAL,SEVERE,CRITICAL};
struct HealthMonitor{MemoryGuardLevel level=MemoryGuardLevel::NORMAL;
 MemoryGuardLevel getMemoryGuardLevel(){return level;}} health;
struct FastBeeFramework{static FastBeeFramework* getInstance(){static FastBeeFramework f;return &f;}
 HealthMonitor* getHealthMonitor(){return &health;}};
constexpr unsigned long TASK_RUN_CYCLE_BUDGET_MS=50;
constexpr uint8_t LOW_PRIORITY_OVERRUN_DISABLE_THRESHOLD=3;
unsigned long defaultExecutionBudget(unsigned long,TaskPriority);
DEFINITIONS
unsigned long defaultExecutionBudget(unsigned long,TaskPriority){return 100;}
struct TaskManager{
FIELDS
 TaskManager(){tasks.reserve(TaskScheduler::MAX_TASKS);isRunning=true;tasksMutex=this;}
 void run();bool removeTask(const String&);bool disableTask(const String&);void clearAllTasks();
 bool addTask(const String&,TaskFunction,void*,unsigned long,TaskPriority,bool=true);
 ScheduledTask& get(const char* name){for(auto& t:tasks)if(!strcmp(t.name,name))return t;std::abort();}
};
METHODS
std::array<int,4096> trace{};size_t count=0;
void record(void* p){assert(mutexDepth==0);trace[count++]=static_cast<int>(reinterpret_cast<intptr_t>(p));}
void slow(void* p){record(p);nowMs+=60;}
void overrun(void* p){record(p);nowMs+=11;}
void mutate(void* p){auto& m=*static_cast<TaskManager*>(p);record(reinterpret_cast<void*>(1));
 assert(m.removeTask("removed"));assert(m.addTask("new",record,reinterpret_cast<void*>(3),0,TaskPriority::PRIORITY_LOW));
 m.run(); // nested dispatch cannot overwrite the outer snapshot
 assert(m.disableTask("mutator"));}
void clear(void* p){record(reinterpret_cast<void*>(4));static_cast<TaskManager*>(p)->clearAllTasks();}
void runNoHeap(TaskManager& m){denyAllocations=true;m.run();denyAllocations=false;
 assert(deniedAllocations==0&&mutexDepth==0&&!m.runInProgress);}
int main(){
 // All 20 slots, stable priority ordering, no new/malloc needed during run.
 TaskManager full;
 for(int i=0;i<20;++i){char name[12];snprintf(name,sizeof(name),"task%d",i);
  assert(full.addTask(name,record,reinterpret_cast<void*>(static_cast<intptr_t>(i)),0,static_cast<TaskPriority>(i%4)));}
 assert(!full.addTask("overflow",record,nullptr,0,TaskPriority::PRIORITY_HIGH));
 for(int round=0;round<100;++round){count=0;runNoHeap(full);assert(count==20);
  size_t pos=0;for(int priority=3;priority>=0;--priority)for(int i=0;i<20;++i)
   if(i%4==priority)assert(trace[pos++]==i);}
 for(auto& t:full.tasks)assert(t.executionCount==100);
 // Removed selected tasks retain the original snapshot; new tasks wait a cycle.
 TaskManager mutation;count=0;
 assert(mutation.addTask("mutator",mutate,&mutation,0,TaskPriority::PRIORITY_HIGH));
 assert(mutation.addTask("removed",record,reinterpret_cast<void*>(2),0,TaskPriority::PRIORITY_LOW));
 runNoHeap(mutation);assert(count==2&&trace[0]==1&&trace[1]==2&&mutation.get("new").executionCount==0);
 runNoHeap(mutation);assert(count==3&&trace[2]==3&&mutation.get("new").executionCount==1);
 // Clear while dispatching preserves this cycle and leaves the next one empty.
 TaskManager cleared;count=0;cleared.addTask("clear",clear,&cleared,0,TaskPriority::PRIORITY_HIGH);
 cleared.addTask("later",record,reinterpret_cast<void*>(5),0,TaskPriority::PRIORITY_LOW);
 runNoHeap(cleared);assert(count==2&&trace[1]==5&&cleared.tasks.empty());runNoHeap(cleared);assert(count==2);
 // Higher-priority overruns defer normal work without losing it.
 TaskManager budget;count=0;nowMs=1000;
 budget.addTask("slow",slow,reinterpret_cast<void*>(6),100,TaskPriority::PRIORITY_HIGH);
 budget.addTask("normal",record,reinterpret_cast<void*>(7),100,TaskPriority::PRIORITY_NORMAL);
 runNoHeap(budget);assert(count==1&&budget.get("normal").deferredCount==1);
 runNoHeap(budget);assert(count==2&&trace[1]==7&&budget.get("normal").executionCount==1);
 // Preserve low-priority consecutive overrun disable and statistics.
 TaskManager overruns;count=0;overruns.addTask("low",overrun,nullptr,0,TaskPriority::PRIORITY_LOW);
 overruns.get("low").executionBudget=10;
 for(int i=0;i<4;++i)runNoHeap(overruns);
 assert(count==3&&!overruns.get("low").enabled&&overruns.get("low").overrunCount==3);
 // Memory guard, null callbacks and interval admission retain their behavior.
 TaskManager guards;count=0;nowMs=1000;
 guards.addTask("high",record,nullptr,100,TaskPriority::PRIORITY_HIGH);
 guards.addTask("normal",record,nullptr,100,TaskPriority::PRIORITY_NORMAL);
 guards.addTask("null",nullptr,nullptr,100,TaskPriority::PRIORITY_CRITICAL);
 health.level=MemoryGuardLevel::SEVERE;runNoHeap(guards);assert(count==1&&guards.get("normal").lastRun==0);
 runNoHeap(guards);assert(count==1&&guards.get("null").executionCount==0);
 health.level=MemoryGuardLevel::NORMAL;runNoHeap(guards);assert(count==2);
 guards.isRunning=false;nowMs+=1000;runNoHeap(guards);assert(count==2);
}
'''.replace("DEFINITIONS", definitions).replace("FIELDS", fields).replace("METHODS", methods)
        # Forward declaration belongs after the production TaskPriority enum.
        cpp = cpp.replace("unsigned long defaultExecutionBudget(unsigned long,TaskPriority);\n", "")
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        with tempfile.TemporaryDirectory(prefix="fastbee-task-noalloc-") as directory:
            base = Path(directory)
            source_path = base / "scheduler.cpp"
            executable = base / ("scheduler.exe" if os.name == "nt" else "scheduler")
            source_path.write_text(cpp, encoding="utf-8")
            compiled = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                       str(source_path), "-o", str(executable)],
                                      capture_output=True, text=True, timeout=40)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
