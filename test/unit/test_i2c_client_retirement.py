"""Exercise production I2C client retirement against the real timed bus manager."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class I2cClientRetirementTests(unittest.TestCase):
    def test_busy_release_reinit_and_owner_address_reuse(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        pm = (ROOT / "src/core/PeripheralManager.cpp").read_text(encoding="utf-8")
        lcd = (ROOT / "src/peripherals/LCDManager.cpp").read_text(encoding="utf-8")
        lcd1602 = (ROOT / "src/peripherals/drivers/LCD1602Driver.cpp").read_text(encoding="utf-8")
        route = (ROOT / "src/network/handlers/PeripheralReadRoutes.cpp").read_text(encoding="utf-8")
        snippets = [definition(pm, "std::map<String, std::unique_ptr<uint8_t>>& i2cOwners()")]
        for method in ("setupHardware", "teardownHardware"):
            body = definition(pm, "bool PeripheralManager::" + method + "(")
            branch = definition(body, "if (config.type == PeripheralType::I2C)")
            snippets.append("bool PeripheralManager::" + method +
                            "(const PeripheralConfig& config) {\n" + branch + "\nreturn false;\n}")
        snippets += [definition(lcd, signature) for signature in (
            "void abortManagedLcdTransfer()", "LCDManager::~LCDManager()",
            "bool LCDManager::deinitialize(const String& expectedId)")]
        snippets += [definition(route, "struct ScanLease") + ";",
                     definition(lcd1602, "void LCD1602Driver::end()")]
        # Exclude LCD command initialization after address detection; the real
        # admission and failed-detection cleanup path are compiled verbatim.
        begin = definition(lcd1602, "bool LCD1602Driver::begin(")
        snippets.append(begin[:begin.index("    // 初始化 LCD（4位模式）")] + "return true;\n}")
        guards = (ROOT / "include/core/AsyncExecTypes.h").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="fastbee-i2c-retirement-") as directory:
            temporary = Path(directory)
            (temporary / "retirement_methods.inc").write_text("\n\n".join(snippets), encoding="utf-8")
            (temporary / "retirement_guard.inc").write_text(
                definition(guards, "class RecursiveMutexGuard") + ";", encoding="utf-8")
            executable = temporary / ("retirement.exe" if os.name == "nt" else "retirement")
            command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                       f"-I{temporary}", f"-I{ROOT / 'test/unit/cpp/io_expander_provider_mocks'}",
                       f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       str(ROOT / "test/unit/cpp/i2c_client_retirement_harness.cpp"),
                       str(ROOT / "src/core/I2CBusManager.cpp"),
                       str(ROOT / "src/peripherals/IoExpander.cpp"),
                       str(ROOT / "src/peripherals/IoExpanderProvider.cpp"), "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
