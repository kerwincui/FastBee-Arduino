"""Run actual Logger allocation, file failure and lock recovery methods offline."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class LoggerRingRuntimeTests(unittest.TestCase):
    def test_real_file_ring_lifecycle(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = (ROOT / "src/systems/LoggerSystem.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/systems/LoggerSystem.h").read_text(encoding="utf-8")
        interface = (ROOT / "include/core/interfaces/ILoggerSystem.h").read_text(encoding="utf-8")
        health = (ROOT / "src/systems/HealthMonitor.cpp").read_text(encoding="utf-8")
        signatures = (
            "LoggerSystem::LoggerSystem(", "LoggerSystem::~LoggerSystem(",
            "bool LoggerSystem::initialize(", "void LoggerSystem::enableFileLogging(",
            "void LoggerSystem::cancelFileLoggingRequest(",
            "void LoggerSystem::logToBuffer(", "size_t LoggerSystem::bufferedDataSize(",
            "void LoggerSystem::flushBufferInternal(", "void LoggerSystem::flushBuffer(",
            "void LoggerSystem::flushBufferIfDue(",
            "void LoggerSystem::log(", "void LoggerSystem::logFormatted(",
            "const char* LoggerSystem::getLevelString(", "int LoggerSystem::espLogCallback(",
            "void LoggerSystem::captureRecentLine(", "size_t LoggerSystem::snapshotRecentLogs(",
            "bool LoggerSystem::lockLogFiles(", "void LoggerSystem::unlockLogFiles(",
            "bool LoggerSystem::rotateLogFile(", "bool LoggerSystem::rotateLogFileInternal(",
            "bool LoggerSystem::clearLogFile(",
            "void LoggerSystem::writeRawLog(",
        )
        with tempfile.TemporaryDirectory(prefix="fastbee-logger-ring-") as directory:
            temporary = Path(directory)
            (temporary / "logger_types.inc").write_text(
                definition(interface, "enum LogLevel") + ";\n" +
                definition(interface, "class ILoggerSystem") + ";\n" +
                definition(header, "class LoggerSystem").replace("private:", "public:") + ";\n",
                encoding="utf-8")
            (temporary / "logger_methods.inc").write_text(
                "\n\n".join(definition(source, signature) for signature in signatures), encoding="utf-8")
            degradation = definition(health, "if (logger.isFileLoggingRequested())")
            (temporary / "logger_degradation.inc").write_text(
                "void degradeFileLogging(LoggerSystem& logger) {\n" + degradation + "\n}", encoding="utf-8")
            executable = temporary / ("logger.exe" if os.name == "nt" else "logger")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", f"-I{temporary}",
                       str(ROOT / "test/unit/cpp/logger_ring_runtime_harness.cpp"), "-o", str(executable)]
            result = subprocess.run(
                command,
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            # Reproduce the old HealthMonitor predicate: PENDING is not active,
            # so an ENABLED-only guard misses the allocation and lets it commit.
            (temporary / "logger_degradation.inc").write_text(
                "void degradeFileLogging(LoggerSystem& logger) {\n" +
                degradation.replace("isFileLoggingRequested()", "isFileLoggingEnabled()") + "\n}",
                encoding="utf-8")
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 86, "Old ENABLED-only degradation predicate must fail the pending-state check")


if __name__ == "__main__":
    unittest.main()
