"""Compile actual worker admission against boot/online memory transitions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class WorkerOnlineAdmissionTests(unittest.TestCase):
    def test_real_gate_preserves_online_web_budget_and_profile_behavior(self):
        production = definition((ROOT / "src/core/PeriphExecWorkers.cpp").read_text(encoding="utf8"),
                                "static bool hasWorkerPoolMemoryHeadroom(")
        worker_header = (ROOT / "include/core/PeriphExecWorkerPool.h").read_text(encoding="utf8")
        worker_begin = worker_header.index("#if defined(BOARD_HAS_PSRAM) && FASTBEE_USE_PSRAM")
        worker_end = worker_header.index("\n#endif", worker_begin) + len("\n#endif")
        worker_stack = worker_header[worker_begin:worker_end]
        harness = r'''
#include <cstdint>
#include <cassert>
#define ESP_PLATFORM 1
bool CHIP_IS_CLASSIC_ESP32=true;
int FASTBEE_USE_PSRAM=0, FASTBEE_ENABLE_ETHERNET=0, FASTBEE_ENABLE_CELLULAR=0;
constexpr int WL_CONNECTED=3, WIFI_AP=2;
struct IPAddress { int value; IPAddress(int a,int b,int c,int d):value(a|b|c|d){}
    bool operator!=(const IPAddress& other)const{return value!=other.value;} };
struct Wifi { int state=0, mode=1; bool apReady=false;
    int status(){return state;} int getMode(){return mode;}
    IPAddress softAPIP(){return IPAddress(apReady?192:0,0,0,0);} } WiFi;
struct Log {void warning(const char*){} template<class... T> void warningf(const char*,T...){} } LOGGER;
struct FastBeeFramework {bool hasNetwork=true; static FastBeeFramework* getInstance(){static FastBeeFramework f;return &f;}
    void* getNetworkManager(){return hasNetwork?this:nullptr;} };
#define SIMPLE_TASK_STACK 4096
struct PeriphExecWorkerPool {
// @production-worker-stack@
static constexpr uint32_t WORKER_COUNT=1;
};
namespace FastBee {struct MemoryBudget {static uint32_t free, largest;
    static uint32_t dram8Free(){return free;}static uint32_t dram8Largest(){return largest;}};
uint32_t MemoryBudget::free=99784,MemoryBudget::largest=38900;}
'''
        harness = harness.replace("// @production-worker-stack@", worker_stack, 1)
        harness += production + r'''
int main(){
    // Reproduces COM16 boot before the 61KB driver allocation.
    assert(!hasWorkerPoolMemoryHeadroom(true));
    WiFi.state=WL_CONNECTED;
    FastBee::MemoryBudget::free=31000;assert(!hasWorkerPoolMemoryHeadroom(false));
    FastBee::MemoryBudget::free=37888;assert(hasWorkerPoolMemoryHeadroom(false));
    FastBee::MemoryBudget::free=37887;assert(!hasWorkerPoolMemoryHeadroom(false));
    FastBee::MemoryBudget::free=40000;FastBee::MemoryBudget::largest=8191;
    assert(!hasWorkerPoolMemoryHeadroom(false));
    FastBee::MemoryBudget::largest=8192;assert(hasWorkerPoolMemoryHeadroom(false));
    WiFi.state=0;WiFi.mode=WIFI_AP;assert(!hasWorkerPoolMemoryHeadroom(false));
    WiFi.apReady=true;assert(hasWorkerPoolMemoryHeadroom(false));
    WiFi.mode=1;WiFi.apReady=false;
    FastBeeFramework::getInstance()->hasNetwork=false;assert(hasWorkerPoolMemoryHeadroom(false));
    FastBeeFramework::getInstance()->hasNetwork=true;
    // C3, PSRAM, and non-WiFi classic configurations retain their admission.
    CHIP_IS_CLASSIC_ESP32=false;FastBee::MemoryBudget::free=12288;assert(hasWorkerPoolMemoryHeadroom(false));
    FastBee::MemoryBudget::free=12287;assert(!hasWorkerPoolMemoryHeadroom(false));
    FastBee::MemoryBudget::free=40000;CHIP_IS_CLASSIC_ESP32=true;
    FASTBEE_USE_PSRAM=1;assert(hasWorkerPoolMemoryHeadroom(false));FASTBEE_USE_PSRAM=0;
    FASTBEE_ENABLE_ETHERNET=1;assert(hasWorkerPoolMemoryHeadroom(false));FASTBEE_ENABLE_ETHERNET=0;
    FASTBEE_ENABLE_CELLULAR=1;assert(hasWorkerPoolMemoryHeadroom(false));
}
'''
        compiler = shutil.which("g++")
        self.assertIsNotNone(compiler)
        with tempfile.TemporaryDirectory(prefix="fastbee-worker-online-") as directory:
            source = Path(directory)/"gate.cpp"
            target = Path(directory)/"gate.exe"
            source.write_text(harness, encoding="utf8")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(target)]
            compiled = subprocess.run(command, capture_output=True, text=True, env=os.environ.copy(), timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout+compiled.stderr)
            result = subprocess.run([str(target)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == "__main__":
    unittest.main()
