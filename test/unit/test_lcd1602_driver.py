"""Compile the complete production LCD1602 driver and real I2C lease manager."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class Lcd1602DriverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise unittest.SkipTest("A C++17 compiler is required")
        cls.inputs = [Path(__file__), ROOT / "src/peripherals/drivers/LCD1602Driver.cpp",
                      ROOT / "include/peripherals/drivers/LCD1602Driver.h",
                      ROOT / "src/core/I2CBusManager.cpp", ROOT / "include/core/I2CBusManager.h",
                      ROOT / "test/unit/cpp/lcd1602_driver_harness.cpp",
                      ROOT / "src/network/handlers/PeripheralRouteHandler.cpp",
                      ROOT / "include/network/LiteralStringMatch.h",
                      ROOT / "src/network/LiteralStringMatch.cpp"]
        for folder in ("test/unit/cpp/lcd1602_mocks",
                       "test/unit/cpp/io_expander_provider_mocks", "test/mocks"):
            cls.inputs.extend((ROOT / folder).rglob("*.h"))
        cls.before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                      for path in cls.inputs}
        cls.temporary = tempfile.TemporaryDirectory(prefix="fastbee-lcd1602-")
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.executable = Path(cls.temporary.name) / ("lcd.exe" if os.name == "nt" else "lcd")
        handler = cpp_function((ROOT / "src/network/handlers/PeripheralRouteHandler.cpp").read_text(encoding="utf-8"),
                               "void PeripheralRouteHandler::handleWritePeripheral(")
        branch = cpp_function(handler, "if (config.type == PeripheralType::LCD1602) {")
        response = handler[handler.rindex("\n    if (success) {"):].rstrip()
        response = response[:response.rindex("}")]
        harness = (ROOT / "test/unit/cpp/lcd1602_driver_harness.cpp").read_text(encoding="utf-8")
        harness = harness.replace("// @lcd1602_http_branch@", branch).replace("// @lcd1602_http_response@", response)
        generated = Path(cls.temporary.name) / "lcd_routes.cpp"
        generated.write_text(harness, encoding="utf-8")
        command = [compiler, "-std=c++17", "-pthread", "-Wall", "-Wextra", "-Werror",
                   "-DFASTBEE_ENABLE_LCD1602=1",
                   f"-I{ROOT / 'test/unit/cpp/lcd1602_mocks'}",
                   f"-I{ROOT / 'test/unit/cpp/io_expander_provider_mocks'}",
                   f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                   str(generated),
                   str(ROOT / "src/peripherals/drivers/LCD1602Driver.cpp"),
                   str(ROOT / "src/core/I2CBusManager.cpp"),
                   str(ROOT / "src/network/LiteralStringMatch.cpp"), "-o", str(cls.executable)]
        result = subprocess.run(command, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        after = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                 for path in cls.inputs}
        if cls.before != after:
            raise AssertionError("LCD1602 production/harness inputs changed during tests")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


def _case(name):
    def run(self):
        self.run_case(name)
    return run


for _name in ("route_clear", "route_backlight", "route_print", "route_busy", "cursor_grouping", "bootstrap", "address_conflict", "init_nack", "init_short_write", "print_nack",
              "line_failures", "data_pulses", "dimensions", "addresses",
              "backlight_failure", "busy_and_retirement", "reinitialize",
              "feature_disabled", "auto_detect", "character_failure",
              "runtime_short_write", "shared_lease", "lost_lease",
              "numeric_bounds", "all_data_bytes", "command_guards",
              "byte_transaction_lock", "bus_begin_failure", "concurrent_busy_failure"):
    setattr(Lcd1602DriverTests, "test_" + _name, _case(_name))


if __name__ == "__main__":
    unittest.main()
