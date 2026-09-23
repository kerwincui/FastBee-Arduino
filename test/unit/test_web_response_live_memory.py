"""Exercise response admission using changing heaps and a stale health sample."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LiveResponseMemoryTests(unittest.TestCase):
    def test_current_heap_controls_admission_and_psram_keeps_dram_reserve(self):
        compiler = shutil.which("g++")
        if not compiler:
            self.skipTest("g++ is required")
        source = (ROOT / "src/network/WebHandlerContext.cpp").read_text(encoding="utf-8")
        start = source.index("static bool canAllocateResponse(size_t requiredSize)")
        end = source.index("\n}\n", start) + 2
        production = source[start:end]
        harness = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
static uint32_t liveFree = 24512, liveLargest = 14324;
static uint32_t cachedLargest = 32, healthReads = 0;
static bool psram = false;
struct SystemHealth { uint32_t largestFreeBlock; };
struct HealthMonitor {
    SystemHealth getHealthStatus() { ++healthReads; return {cachedLargest}; }
};
struct FastBeeFramework {
    static FastBeeFramework* getInstance() { static FastBeeFramework f; return &f; }
    HealthMonitor* getHealthMonitor() { static HealthMonitor h; return &h; }
};
namespace FastBee {
struct MemoryBudget {
    static uint32_t dram8Free() { return liveFree; }
    static uint32_t dram8Largest() { return liveLargest; }
    static uint32_t dramFree() { return liveFree + 28000; }
    static uint32_t dramLargest() { return liveLargest + 28000; }
};
bool psramAvailableForJson(size_t) { return psram; }
}
template<typename... T> void ets_printf(const char*, T...) {}
PRODUCTION
int main() {
    // The previous cold-load sample is exhausted; the current heap is healthy.
    assert(canAllocateResponse(107));
    assert(healthReads == 0);
    // A stale healthy report must not admit a response on a newly exhausted heap.
    cachedLargest = 14324;
    liveLargest = 512;
    assert(!canAllocateResponse(107));
    liveLargest = 619;
    assert(canAllocateResponse(107));
    assert(!canAllocateResponse(108));
    // External RAM is only a fallback while byte-addressable internal reserves hold.
    psram = true;
    liveFree = 2048; liveLargest = 1024;
    assert(canAllocateResponse(8192));
    liveFree = 2047;
    assert(!canAllocateResponse(8192));
    liveFree = 2048; liveLargest = 1023;
    assert(!canAllocateResponse(8192));
    psram = false; liveLargest = 1024;
    assert(!canAllocateResponse(8192));
    assert(healthReads == 0);
}
'''.replace("PRODUCTION", production)
        with tempfile.TemporaryDirectory(prefix="fastbee-live-response-") as directory:
            cpp = Path(directory) / "test.cpp"
            binary = Path(directory) / "test.exe"
            cpp.write_text(harness, encoding="utf-8")
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                    str(cpp), "-o", str(binary)], capture_output=True,
                                   text=True, env=environment, timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    env=environment, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
