"""Compile the production IRQ drain with a refilling queue and real PM mutex."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class PeripheralInterruptBudgetTests(unittest.TestCase):
    def test_continuous_irq_slow_pm_and_callback_budget(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        json_headers = list((ROOT / ".pio/libdeps").glob("*/ArduinoJson/src/ArduinoJson.h"))
        if not json_headers:
            self.skipTest("Install ArduinoJson first")
        source = (ROOT / "src/core/PeripheralManagerDiagnostics.cpp").read_text(encoding="utf-8")
        header = (ROOT / "include/core/PeripheralManager.h").read_text(encoding="utf-8")
        guards = (ROOT / "include/core/AsyncExecTypes.h").read_text(encoding="utf-8")
        methods = "\n\n".join(definition(source, signature) for signature in (
            "void PeripheralManager::processInterruptQueue()",
            "bool PeripheralManager::handleInterrupt()",
            "bool PeripheralManager::shouldDispatchIsrEvent(",
            "uint16_t PeripheralManager::isrDebounceMsForType(",
            "String PeripheralManager::getPinPeripheralId(",
        ))
        declarations = header[header.index("    // 中断事件队列"):header.index("    // 检查引脚有效性")]
        with tempfile.TemporaryDirectory(prefix="fastbee-irq-budget-") as directory:
            temporary = Path(directory)
            (temporary / "irq_methods.inc").write_text(methods, encoding="utf-8")
            (temporary / "irq_members.inc").write_text(declarations, encoding="utf-8")
            (temporary / "irq_guard.inc").write_text(
                definition(guards, "class RecursiveMutexGuard") + ";", encoding="utf-8")
            executable = temporary / ("irq.exe" if os.name == "nt" else "irq")
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                       f"-I{temporary}", f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       f"-I{json_headers[0].parent}",
                       str(ROOT / "test/unit/cpp/peripheral_interrupt_budget_harness.cpp"),
                       "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
