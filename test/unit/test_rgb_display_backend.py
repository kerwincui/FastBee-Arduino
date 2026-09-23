"""Execute the production RGB backend against deterministic bus/LCD fakes."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_screen_panel_contract import braced_block

ROOT = pathlib.Path(__file__).resolve().parents[2]


class RgbDisplayBackendTests(unittest.TestCase):
    def test_persisted_display_reserves_control_and_both_touch_addresses(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        manager = (ROOT / "src/core/PeripheralManager.cpp").read_text(encoding="utf-8")
        definitions = []
        for signature in ("uint8_t managedI2cAddress(const PeripheralConfig& config)",
                          "bool managedI2cClaimsAddress(const PeripheralConfig& config, uint8_t address)",
                          "bool managedI2cAddressesOverlap(const PeripheralConfig& left, const PeripheralConfig& right)"):
            definitions.append(signature + " {" + braced_block(manager, signature) + "}")
        source = r'''
#include <cassert>
#include <math.h>
#include <cstring>
#include <ArduinoJson.h>
#include "core/PeripheralConfig.h"
DEFINITIONS
int main() {
    PeripheralConfig display; display.type=PeripheralType::SCREEN_DISPLAY; initializeDisplayDefaults(display);
    std::strcpy(display.params.display.backend,"rgb"); std::strcpy(display.params.display.touchBackend,"gt911");
    std::strcpy(display.params.display.controlBackend,"ch32v003"); display.params.display.controlAddress=0x24;
    PeripheralConfig sensor; sensor.type=PeripheralType::SENSOR_GENERIC; initializeSensorDefaults(sensor);
    for(uint8_t address : {uint8_t(0x24),uint8_t(0x5d),uint8_t(0x14)}) {
        sensor.params.sensor.address=address;
        assert(managedI2cAddressesOverlap(display,sensor) && managedI2cAddressesOverlap(sensor,display));
    }
    sensor.params.sensor.address=0x44;
    assert(!managedI2cAddressesOverlap(display,sensor) && !managedI2cAddressesOverlap(sensor,display));
    PeripheralConfig lcd; lcd.type=PeripheralType::LCD1602; lcd.params.lcd1602.address=0;
    assert(managedI2cAddressesOverlap(display,lcd));
    std::strcpy(display.params.display.touchBackend,"none");
    assert(!managedI2cClaimsAddress(display,0x5d) && !managedI2cClaimsAddress(display,0x14));
    assert(managedI2cClaimsAddress(display,0x24));
    std::strcpy(display.params.display.controlBackend,"none");
    assert(!managedI2cClaimsAddress(display,0x24));
}
'''.replace("DEFINITIONS", "\n".join(definitions))
        with tempfile.TemporaryDirectory(prefix="fastbee-rgb-address-") as directory:
            temporary = pathlib.Path(directory)
            cpp = temporary / "addresses.cpp"
            cpp.write_text(source, encoding="utf-8")
            executable = temporary / ("addresses.exe" if os.name == "nt" else "addresses")
            build = subprocess.run([compiler, "-std=c++17", "-DUNIT_TEST", "-DARDUINOJSON_ENABLE_STD_STRING=1",
                                    "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0", f"-I{ROOT / 'test/unit/cpp/mocks'}",
                                    f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}", str(cpp), "-o", str(executable)],
                                   capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_production_backend_lifecycle_rotation_and_masked_control(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists():
            compiler = str(fallback)
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        config = (ROOT / "include/core/PeripheralConfig.h").read_text(encoding="utf-8")
        backend = (ROOT / "include/screen/ScreenBackend.h").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="fastbee-rgb-backend-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "production_display_config.inc").write_text(
                "struct DisplayConfigParams {" + braced_block(config, "struct DisplayConfigParams") + "};", encoding="utf-8")
            for name in ("core/FeatureFlags.h", "core/I2CBusManager.h", "Arduino.h", "esp_heap_caps.h",
                         "esp_lcd_panel_ops.h", "esp_lcd_panel_rgb.h", "esp_rom_sys.h", "esp_timer.h",
                         "driver/gpio.h", "systems/LoggerSystem.h", "sdkconfig.h"):
                target = temporary / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('#pragma once\n', encoding="utf-8")
            (temporary / "screen").mkdir()
            (temporary / "screen/ScreenBackend.h").write_text(
                backend.replace('#include "core/PeripheralConfig.h"', ''), encoding="utf-8")
            executable = temporary / ("rgb.exe" if os.name == "nt" else "rgb")
            env = dict(os.environ)
            env["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            cache_safe_flags = ["-DCONFIG_LCD_RGB_ISR_IRAM_SAFE=1", "-DCONFIG_SPIRAM_XIP_FROM_PSRAM=1",
                                "-DCONFIG_SPIRAM_FETCH_INSTRUCTIONS=1", "-DCONFIG_SPIRAM_RODATA=1"]
            sdk_profiles = (
                [],
                ["-DCONFIG_LCD_RGB_ISR_IRAM_SAFE=1"],
                ["-DCONFIG_SPIRAM_XIP_FROM_PSRAM=1", "-DCONFIG_SPIRAM_FETCH_INSTRUCTIONS=1",
                 "-DCONFIG_SPIRAM_RODATA=1"],
                ["-DCONFIG_LCD_RGB_ISR_IRAM_SAFE=1", "-DCONFIG_SPIRAM_FETCH_INSTRUCTIONS=1",
                 "-DCONFIG_SPIRAM_RODATA=1"],
                cache_safe_flags,
            )
            for sdk_flags in sdk_profiles:
                with self.subTest(profile=sdk_flags or ["direct-edma"]):
                    self._run_backend(compiler, temporary, executable, env, sdk_flags,
                                      "RGB backend: lifecycle and memory scenarios passed")

            # Telemetry is compiled out by default. This explicit build proves
            # the public SDK callback path attaches, reports, and detaches.
            self._run_backend(compiler, temporary, executable, env,
                              cache_safe_flags + ["-DFASTBEE_RGB_SCAN_TELEMETRY=1"],
                              "RGB backend: lifecycle and memory scenarios passed")

            # Reproduce the pre-r14/r15 ~95 KB admission window. A required
            # 16-line pair (30,720 B) fits the new reserve, while the required
            # 32-line pair (61,440 B) must be rejected before allocation.
            for lines, expected in ((16, "RGB budget: required16 accepted"),
                                    (32, "RGB budget: required32 rejected")):
                self._run_backend(
                    compiler, temporary, executable, env,
                    cache_safe_flags + ["-DFASTBEE_RGB_BUDGET_ONLY=1",
                                        "-DFASTBEE_RGB_REQUIRE_BOUNCE=1",
                                        f"-DFASTBEE_RGB_BOUNCE_LINES={lines}"],
                    expected)

    def _run_backend(self, compiler, temporary, executable, env, sdk_flags, expected):
        build = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", *sdk_flags,
                                "-DFASTBEE_ENABLE_SCREEN_UI=1", "-DARDUINO_ARCH_ESP32=1", "-DCONFIG_IDF_TARGET_ESP32S3=1",
                                "-DFASTBEE_USE_PSRAM=1", "-DFASTBEE_RGB_RESTART_ON_DEMAND=1",
                                f"-I{temporary}", f"-I{ROOT / 'src/screen'}", f"-I{ROOT / 'include'}",
                                str(ROOT / "test/unit/cpp/rgb_display_backend_harness.cpp"), "-o", str(executable)],
                               capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60, env=env)
        self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, env=env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(expected, result.stdout)


if __name__ == "__main__":
    unittest.main()
