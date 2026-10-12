"""Real optional recent-log methods, allocation cancellation and concurrent snapshots."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class LoggerRecentRuntimeTests(unittest.TestCase):
    def test_recent_buffer_lifetime_with_real_snapshot_and_disable(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "src/systems/LoggerSystem.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/systems/LoggerSystem.h").read_text(encoding="utf-8")
        interface = (ROOT / "include/core/interfaces/ILoggerSystem.h").read_text(encoding="utf-8")
        signatures = (
            "LoggerSystem::LoggerSystem(", "LoggerSystem::~LoggerSystem(",
            "bool LoggerSystem::initialize(", "void LoggerSystem::enableFileLogging(",
            "void LoggerSystem::cancelFileLoggingRequest(", "void LoggerSystem::enableLogViewer(",
            "void LoggerSystem::appendRecentLine(", "size_t LoggerSystem::recentLogLineCount(",
            "void LoggerSystem::logToBuffer(", "size_t LoggerSystem::bufferedDataSize(",
            "void LoggerSystem::flushBufferInternal(", "void LoggerSystem::flushBuffer(",
            "void LoggerSystem::flushBufferIfDue(", "void LoggerSystem::log(",
            "void LoggerSystem::logFormatted(", "const char* LoggerSystem::getLevelString(",
            "int LoggerSystem::espLogCallback(", "void LoggerSystem::captureRecentLine(",
            "size_t LoggerSystem::snapshotRecentLogs(", "bool LoggerSystem::lockLogFiles(",
            "void LoggerSystem::unlockLogFiles(", "FastBee::FeatureInstanceSnapshot LoggerSystem::observeFeatureInstance(",
            "bool LoggerSystem::rotateLogFile(", "bool LoggerSystem::rotateLogFileInternal(",
            "bool LoggerSystem::clearLogFile(", "void LoggerSystem::writeRawLog(",
        )
        base = (ROOT / "test/unit/cpp/logger_ring_runtime_harness.cpp").read_text(encoding="utf-8")
        base = base[:base.index("void formattedLog(")]
        base = base.replace('#include <vector>', '#include <vector>\n#include <chrono>\n#include <mutex>\n#include <thread>')
        allocation_start = base.index('static void* ringAllocation')
        allocation_end = base.index('struct Mutex')
        base = base[:allocation_start] + r'''
static void* allocations[8] = {};
static size_t allocationCount=0,releaseCount=0;
bool retainedAllocation(void* pointer) {
    for(auto value:allocations)if(value==pointer)return true;
    return false;
}
void* operator new(size_t count,const std::nothrow_t&) noexcept {
    if(failAllocation)return nullptr;
    if(duringAllocation){auto hook=std::move(duringAllocation);duringAllocation={};hook();}
    void* result=std::malloc(count);
    if(result){for(auto& value:allocations)if(!value){value=result;++allocationCount;return result;}}
    assert(!result);return result;
}
void operator delete(void* pointer) noexcept {
    if(pointer){for(auto& value:allocations)if(value==pointer){value=nullptr;++releaseCount;break;}}
    std::free(pointer);
}
void operator delete(void* pointer,size_t) noexcept {::operator delete(pointer);}
''' + base[allocation_end:]
        base = base.replace('struct Mutex { bool held = false, reject = false; };',
                            'struct Mutex { std::timed_mutex value; bool held = false, reject = false; };')
        base = base.replace('if (!value || value->held || value->reject) return 0;\n    value->held = true;',
                            'if (!value || value->reject) return 0;\n'
                            '    if (!value->value.try_lock_for(std::chrono::milliseconds(timeout))) return 0;\n'
                            '    value->held = true;')
        base = base.replace('value->held = false; }', 'value->held = false; value->value.unlock(); }')
        base = base.replace('#include "logger_degradation.inc"', '')
        base = base.replace('#include "logger_methods.inc"', r'''
static std::function<void()> duringCopy;
void* recentTestMemcpy(void* destination,const void* source,size_t count) {
    if(duringCopy){auto hook=std::move(duringCopy);duringCopy={};hook();}
    return std::memcpy(destination,source,count);
}
#define memcpy recentTestMemcpy
#include "logger_methods.inc"
#undef memcpy
''')
        with tempfile.TemporaryDirectory(prefix="fastbee-logger-recent-") as directory:
            temporary = Path(directory)
            (temporary / "logger_recent_base.inc").write_text(base, encoding="utf-8")
            (temporary / "logger_types.inc").write_text(
                definition(interface, "enum LogLevel") + ";\n" +
                definition(interface, "class ILoggerSystem") + ";\n" +
                definition(header, "class LoggerSystem").replace("private:", "public:") + ";\n",
                encoding="utf-8")
            (temporary / "logger_methods.inc").write_text(
                "\n\n".join(definition(source, signature) for signature in signatures), encoding="utf-8")
            executable = temporary / ("recent.exe" if os.name == "nt" else "recent")
            result = subprocess.run([
                compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                f"-I{temporary}", f"-I{ROOT / 'include'}",
                str(ROOT / "test/unit/cpp/logger_recent_runtime_harness.cpp"), "-o", str(executable),
            ], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
