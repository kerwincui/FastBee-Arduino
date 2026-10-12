"""Exercise the actual route guard against a concurrent log flush."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class LogRouteContentionTests(unittest.TestCase):
    def test_read_waits_for_flush_and_timeout_does_not_unlock_writer(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "src/network/handlers/LogRouteHandler.cpp").read_text(encoding="utf-8")
        guard = definition(source, "class ScopedLogFileAccess") + ";\n"
        harness = r'''
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
using namespace std::chrono_literals;
struct Logger {
    std::timed_mutex files;
    std::atomic<int> unlocks{0};
    bool lockLogFiles(uint32_t ms) { return files.try_lock_for(std::chrono::milliseconds(ms)); }
    void unlockLogFiles() { ++unlocks; files.unlock(); }
} LOGGER;
@GUARD@
int main() {
    std::atomic<bool> writing{false};
    std::thread flush([&] {
        LOGGER.files.lock(); writing=true;
        std::this_thread::sleep_for(250ms);
        LOGGER.files.unlock();
    });
    while (!writing) std::this_thread::yield();
    const auto started=std::chrono::steady_clock::now();
    {
        ScopedLogFileAccess read;
        assert(read.locked());
        assert(std::chrono::steady_clock::now()-started>=150ms);
        bool excluded=false;
        std::thread contender([&] {
            excluded=!LOGGER.files.try_lock();
            if (!excluded) LOGGER.files.unlock();
        });
        contender.join(); assert(excluded);
    }
    flush.join(); assert(LOGGER.unlocks==1);
    assert(LOGGER.files.try_lock()); LOGGER.files.unlock();

    writing=false;
    std::atomic<bool> finish{false};
    std::thread stuck([&] {
        LOGGER.files.lock(); writing=true;
        while (!finish) std::this_thread::sleep_for(1ms);
        LOGGER.files.unlock();
    });
    while (!writing) std::this_thread::yield();
    const auto blocked=std::chrono::steady_clock::now();
    {
        ScopedLogFileAccess read;
        assert(!read.locked());
        assert(std::chrono::steady_clock::now()-blocked<4500ms);
    }
    assert(LOGGER.unlocks==1); // A failed reader never releases the writer lock.
    finish=true; stuck.join();
    {
        ScopedLogFileAccess recovered;
        assert(recovered.locked());
    }
    assert(LOGGER.unlocks==2);
}
'''.replace("@GUARD@", guard)
        with tempfile.TemporaryDirectory(prefix="fastbee-log-contention-") as directory:
            cpp = Path(directory) / "contention.cpp"
            cpp.write_text(harness, encoding="utf-8")
            executable = Path(directory) / ("contention.exe" if os.name == "nt" else "contention")
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                 str(cpp), "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
