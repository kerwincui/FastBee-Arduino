"""Exercise the real PSRAM logger drain with a deterministic task scheduler."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class LoggerAsyncFlushTests(unittest.TestCase):
    def test_periodic_drain_admission_and_lifecycle(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "src/systems/LoggerSystem.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/systems/LoggerSystem.h").read_text(encoding="utf-8")
        interface = (ROOT / "include/core/interfaces/ILoggerSystem.h").read_text(encoding="utf-8")
        signatures = (
            "LoggerSystem::LoggerSystem(", "LoggerSystem::~LoggerSystem(",
            "bool LoggerSystem::initialize(", "void LoggerSystem::enableFileLogging(",
            "void LoggerSystem::cancelFileLoggingRequest(",
            "void LoggerSystem::logToBuffer(", "size_t LoggerSystem::bufferedDataSize(",
            "void LoggerSystem::flushBufferInternal(", "void LoggerSystem::flushBuffer(",
            "void LoggerSystem::flushBufferIfDue(", "void LoggerSystem::flushTask(",
            "void LoggerSystem::log(", "void LoggerSystem::logFormatted(",
            "const char* LoggerSystem::getLevelString(", "int LoggerSystem::espLogCallback(",
            "void LoggerSystem::captureRecentLine(", "size_t LoggerSystem::snapshotRecentLogs(",
            "bool LoggerSystem::lockLogFiles(", "void LoggerSystem::unlockLogFiles(",
            "bool LoggerSystem::rotateLogFile(", "bool LoggerSystem::rotateLogFileInternal(",
            "bool LoggerSystem::clearLogFile(", "void LoggerSystem::writeRawLog(",
        )
        # Reuse the existing platform fixtures; production methods are still
        # extracted verbatim, and the original no-PSRAM test remains unchanged.
        base = (ROOT / "test/unit/cpp/logger_ring_runtime_harness.cpp").read_text(encoding="utf-8")
        base = base[:base.index("void formattedLog(")]
        base = base.replace("void delay(int) {}", "void runPendingTask();\nvoid delay(int) { runPendingTask(); }")
        base = base.replace("size_t dram8Free() { return freeBytes; }",
                            "size_t dram8Free() { return freeBytes; }\n"
                            "static size_t largestBytes = 32000;\n"
                            "size_t dram8Largest() { return largestBytes; }")
        base = base.replace('#include "logger_types.inc"',
                            '#include "logger_async_scheduler.inc"\n#include "logger_types.inc"')
        base = base.replace('#include "logger_degradation.inc"', "")
        base = base.replace("struct File {", "static std::function<void()> duringWrite;\nstruct File {")
        base = base.replace("written.append(reinterpret_cast<const char*>(data), count);",
                            "if (duringWrite) { auto hook = std::move(duringWrite); duringWrite = {}; hook(); } "
                            "written.append(reinterpret_cast<const char*>(data), count);")
        with tempfile.TemporaryDirectory(prefix="fastbee-logger-async-") as directory:
            temporary = Path(directory)
            (temporary / "logger_async_base.inc").write_text(base, encoding="utf-8")
            (temporary / "logger_types.inc").write_text(
                definition(interface, "enum LogLevel") + ";\n" +
                definition(interface, "class ILoggerSystem") + ";\n" +
                definition(header, "class LoggerSystem").replace("private:", "public:") + ";\n",
                encoding="utf-8")
            (temporary / "logger_methods.inc").write_text(
                "\n\n".join(definition(source, signature) for signature in signatures), encoding="utf-8")
            executable = temporary / ("logger.exe" if os.name == "nt" else "logger")
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DFASTBEE_USE_PSRAM=1",
                 f"-I{temporary}", f"-I{ROOT / 'test/unit/cpp'}",
                 str(ROOT / "test/unit/cpp/logger_async_flush_harness.cpp"), "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
