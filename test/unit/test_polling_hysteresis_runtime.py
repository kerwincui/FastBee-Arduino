"""Execute the real polling guard across chip thresholds and state transitions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]

HARNESS = r'''
void require(bool condition, const char* label) {
    if (!condition) { std::cerr << label << '\n'; std::exit(86); }
}
int main() {
    const auto normal = MemoryGuardLevel::NORMAL;
    const uint32_t safeFree = 65536, safeBlock = 32768;
    for (bool suspended : {false, true}) {
        require(shouldSuspendBackgroundPolling(MemoryGuardLevel::SEVERE,
                    safeFree, safeBlock, 0, suspended), "SEVERE must suspend");
        require(shouldSuspendBackgroundPolling(MemoryGuardLevel::CRITICAL,
                    safeFree, safeBlock, 0, suspended), "CRITICAL must suspend");
        require(!shouldSuspendBackgroundPolling(normal, safeFree, safeBlock,
                    0, suspended), "safe memory must run");
        require(shouldSuspendBackgroundPolling(normal, 8191, safeBlock,
                    0, suspended), "free heap hard floor");
        require(shouldSuspendBackgroundPolling(normal, safeFree, 6143,
                    0, suspended), "largest block hard floor");
        require(shouldSuspendBackgroundPolling(normal,
                    WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES - 1, safeBlock,
                    0, suspended), "chip free suspend floor");
        require(shouldSuspendBackgroundPolling(normal, safeFree,
                    WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES - 1,
                    0, suspended), "chip largest suspend floor");
        require(shouldSuspendBackgroundPolling(normal, safeFree, 12287,
                    65, suspended), "fragmentation pause cannot be bypassed on resume");
    }
    require(!shouldSuspendBackgroundPolling(normal,
                WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES, safeBlock, 0, false),
            "exact free suspend boundary");
    require(!shouldSuspendBackgroundPolling(normal, safeFree,
                WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES, 0, false),
            "exact largest suspend boundary");
    require(shouldSuspendBackgroundPolling(normal,
                WEB_RESERVE_RESUME_FREE_HEAP_BYTES - 1, safeBlock, 0, true),
            "resume free boundary minus one");
    require(shouldSuspendBackgroundPolling(normal, safeFree,
                WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES - 1, 0, true),
            "resume largest boundary minus one");
    require(!shouldSuspendBackgroundPolling(normal,
                WEB_RESERVE_RESUME_FREE_HEAP_BYTES,
                WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES, 0, true),
            "exact recovery boundary with low fragmentation");

    // At unchanged memory, one transition may establish suspension but the
    // next call must not reverse it. Exercise both initial states, all levels,
    // and neighborhoods of independent guard boundaries.
    const uint32_t heaps[] = {8191, 8192,
        WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES - 1,
        WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES,
        WEB_RESERVE_RESUME_FREE_HEAP_BYTES - 1,
        WEB_RESERVE_RESUME_FREE_HEAP_BYTES, 33348, safeFree};
    const uint32_t blocks[] = {6143, 6144,
        WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES - 1,
        WEB_RESERVE_SUSPEND_LARGEST_BLOCK_BYTES,
        WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES - 1,
        WEB_RESERVE_RESUME_LARGEST_BLOCK_BYTES, 11764, 12287, 12288, safeBlock};
    for (auto level : {normal, MemoryGuardLevel::WARN,
                       MemoryGuardLevel::SEVERE, MemoryGuardLevel::CRITICAL}) {
        for (auto heap : heaps) for (auto block : blocks) {
            for (uint8_t frag : {0, 64, 65, 66, 100}) {
                for (bool initial : {false, true}) {
                    const bool once = shouldSuspendBackgroundPolling(level, heap, block, frag, initial);
                    require(shouldSuspendBackgroundPolling(level, heap, block, frag, once) == once,
                            "unchanged memory must not oscillate");
                }
            }
        }
    }
#if CHIP_IS_ESP32C3 && !defined(CONFIG_SPIRAM_USE)
    // Actual COM4 observation that previously alternated suspend/resume.
    bool state = true;
    for (int cycle = 0; cycle < 100; ++cycle) {
        state = shouldSuspendBackgroundPolling(normal, 33348, 11764, 65, state);
        require(state, "COM4 observed fragmented heap must remain suspended");
    }
    require(!shouldSuspendBackgroundPolling(normal, 33348, 11764, 64, true),
            "COM4 fragmentation recovery below 65 percent");
    require(!shouldSuspendBackgroundPolling(normal, 33348, 12288, 65, true),
            "COM4 block recovery at 12288 bytes");
    state = shouldSuspendBackgroundPolling(normal, 33348, 12288, 65, state);
    for (int cycle = 0; cycle < 100; ++cycle) {
        state = shouldSuspendBackgroundPolling(normal, 33348, 12288, 65, state);
        require(!state, "COM4 recovered memory must remain running");
    }
#endif
}
'''


class PollingHysteresisRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.compiler = shutil.which("g++") or shutil.which("clang++")
        if not self.compiler:
            self.skipTest("A C++17 compiler is required")
        scheduler = (ROOT / "src/core/PeriphExecScheduler.cpp").read_text(encoding="utf-8")
        health = (ROOT / "include/systems/HealthMonitor.h").read_text(encoding="utf-8")
        self.namespace = definition(scheduler, "namespace {")
        self.enum = definition(health, "enum class MemoryGuardLevel") + ";"

    def compile_and_run(self, macros, namespace=None):
        with tempfile.TemporaryDirectory(prefix="fastbee-polling-hysteresis-") as directory:
            temporary = Path(directory)
            source = temporary / "guard.cpp"
            source.write_text(
                '#include <cstddef>\n#include <cstdlib>\n#include <iostream>\n'
                '#include "core/ChipConfig.h"\n' + self.enum + "\n" +
                (self.namespace if namespace is None else namespace) + "\n" + HARNESS,
                encoding="utf-8")
            binary = temporary / ("guard.exe" if os.name == "nt" else "guard")
            compiled = subprocess.run(
                [self.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 f"-I{ROOT / 'include'}", *[f"-D{macro}=1" for macro in macros],
                 str(source), "-o", str(binary)], capture_output=True,
                text=True, encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            return subprocess.run([str(binary)], capture_output=True,
                                  text=True, timeout=15)

    def test_production_guards_across_chip_and_psram_macros(self):
        variants = (
            ("CONFIG_IDF_TARGET_ESP32C3",),
            ("CONFIG_IDF_TARGET_ESP32",),
            ("CONFIG_IDF_TARGET_ESP32", "CONFIG_SPIRAM_USE"),
            ("CONFIG_IDF_TARGET_ESP32C3", "CONFIG_SPIRAM_USE", "CONFIG_SPIRAM_TYPE_IRAM"),
            ("CONFIG_IDF_TARGET_ESP32C6",),
            ("CONFIG_IDF_TARGET_ESP32C6", "CONFIG_SPIRAM_USE"),
            ("CONFIG_IDF_TARGET_ESP32S3",),
            ("CONFIG_IDF_TARGET_ESP32S3", "CONFIG_SPIRAM_USE"),
            ("CONFIG_IDF_TARGET_ESP32S3", "CONFIG_SPIRAM_USE", "CONFIG_SPIRAM_TYPE_OCT"),
            (),
        )
        for macros in variants:
            with self.subTest(macros=macros):
                result = self.compile_and_run(macros)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_old_early_resume_mutation_reproduces_failure(self):
        # Restore the old ordering without copying the production predicate:
        # the old resume branch returns before the fragmented-heap check.
        resume = definition(self.namespace, "if (currentlySuspended)")
        old = self.namespace.replace(resume, "", 1)
        insertion = old.index("if (freeHeap < WEB_RESERVE_SUSPEND_FREE_HEAP_BYTES)")
        old = old[:insertion] + resume + "\n" + old[insertion:]
        result = self.compile_and_run(("CONFIG_IDF_TARGET_ESP32C3",), old)
        self.assertEqual(result.returncode, 86, result.stdout + result.stderr)
        self.assertIn("fragmentation pause cannot be bypassed on resume", result.stderr)


if __name__ == "__main__":
    unittest.main()
