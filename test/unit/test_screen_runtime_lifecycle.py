"""Focused contracts for screen backend ownership and runtime teardown."""

from __future__ import annotations

import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEVICE_HEADER = ROOT / "include/screen/ScreenDevice.h"
DEVICE_SOURCE = ROOT / "src/screen/ScreenDevice.cpp"
RUNTIME_HEADER = ROOT / "include/screen/ScreenRuntime.h"
RUNTIME_SOURCE = ROOT / "src/screen/ScreenRuntime.cpp"
PANEL_SOURCE = ROOT / "src/core/ScreenPanelManager.cpp"
PERIPHERAL_SOURCE = ROOT / "src/core/PeripheralManager.cpp"
PERIPHERAL_HEADER = ROOT / "include/core/PeripheralConfig.h"
PERIPHERAL_PERSISTENCE = ROOT / "src/core/PeripheralManagerPersistence.cpp"
PERIPHERAL_ROUTE = ROOT / "src/network/handlers/PeripheralRouteHandler.cpp"
PERIPHERAL_ROUTE_MAPPER = ROOT / "src/core/PeripheralRouteMapper.cpp"
PERIPHERAL_PARAM_APPLY = ROOT / "src/core/PeripheralParamApply.cpp"
FRAMEWORK_SOURCE = ROOT / "src/core/FastBeeFramework.cpp"
FEATURE_SOURCE = ROOT / "src/core/RuntimeFeatureManager.cpp"
SPI_SOURCE = ROOT / "src/screen/SpiTftDisplayBackend.cpp"
TOUCH_SOURCE = ROOT / "src/screen/Gt911TouchBackend.cpp"
HARNESS = ROOT / "test/unit/cpp/screen_device_harness.cpp"
GEOMETRY_HARNESS = ROOT / "test/unit/cpp/screen_geometry_harness.cpp"
MOCKS = ROOT / "test/unit/cpp/screen_device_mocks"


def braced_block(source: str, marker: str) -> str:
    marker_at = source.find(marker)
    if marker_at < 0:
        raise AssertionError(f"missing source marker {marker!r}")
    opening = source.find("{", marker_at + len(marker))
    if opening < 0:
        raise AssertionError(f"missing block after {marker!r}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


class ScreenRuntimeLifecycleTests(unittest.TestCase):
    def test_task_handle_survives_cleanup_and_state_lock_contention(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required")
        runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
        definitions = '\n'.join(signature + ' {' + braced_block(runtime, signature) + '}'
                                for signature in ("void finishTask()", "void screenTask(void*)"))
        source = r'''
#include <cassert>
#include <string>
using String=std::string;
enum class ScreenSystemAction { NONE, SLEEP };
struct RuntimeState {
 bool running=true,sleeping=true; int pendingSystemValue=1,systemActionCapabilities=1,allocated=1024;
 ScreenSystemAction pendingSystemAction=ScreenSystemAction::SLEEP;
 void* task=reinterpret_cast<void*>(1); String lastError;
} state;
RuntimeState& runtimeState(){return state;}
int lockFailures=0,attempts=0,waits=0,deletes=0; bool workerReturned=false;
struct StateLock { bool acquired; StateLock():acquired(lockFailures==0){if(lockFailures)--lockFailures;}
 bool locked() const{return acquired;} };
#define pdMS_TO_TICKS(x) (x)
void vTaskDelay(int){assert(state.task && deletes==0);++waits;}
void vTaskDelete(void*){assert(state.task==nullptr && attempts==3);++deletes;}
void runScreenTask(){workerReturned=true;}
struct ScreenDevice {
 static ScreenDevice& instance(){static ScreenDevice device;return device;}
 bool deactivate(String* error){
  assert(workerReturned && state.task && deletes==0 && !state.running);
  if(++attempts<3){*error="SPI_BUSY";return false;}
  lockFailures=2;return true;
 }
};
bool deactivateScreenDeviceResources(String* error){return ScreenDevice::instance().deactivate(error);}
DEFINITIONS
int main(){screenTask(nullptr);assert(attempts==3 && waits==4 && deletes==1);
 assert(!state.running && !state.sleeping && state.allocated==0 && state.pendingSystemValue==0);}
'''.replace('DEFINITIONS', definitions)
        with tempfile.TemporaryDirectory(prefix="fastbee-screen-stop-") as tmp:
            cpp = pathlib.Path(tmp) / 'stop.cpp'
            binary = pathlib.Path(tmp) / ('stop.exe' if os.name == 'nt' else 'stop')
            cpp.write_text(source, encoding='utf-8')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                    str(cpp), '-o', str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_rgb_memory_admission_requires_full_contiguous_psram_frame(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required")
        runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
        definitions = []
        for constant in ("TASK_STACK_BYTES", "SCREEN_INTERNAL_RESERVE_PSRAM",
                         "SCREEN_INTERNAL_RESERVE_NO_PSRAM", "SCREEN_PSRAM_RESERVE"):
            definitions.append(re.search(r"constexpr [^\n]+ " + constant + r" = [^;]+;", runtime).group(0))
        for signature in ("size_t screenFramebufferBytes(const DisplayConfigParams* config)",
                          "bool screenMemoryAvailable(const DisplayConfigParams* config, String& error)"):
            definitions.append(signature + " {" + braced_block(runtime, signature) + "}")
        source = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
using String=std::string;
struct DisplayConfigParams { char backend[16]; uint16_t width=480,height=480; uint8_t bufferLines=16; };
struct ScreenPanelSnapshot { char representativePayload[8192]; };
enum { MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2, MALLOC_CAP_SPIRAM=4 };
size_t internalFree=128*1024,internalLargest=64*1024,psramTotal=8*1024*1024;
size_t psramFree=1024*1024,psramLargest=1024*1024;
size_t heap_caps_get_free_size(int caps){return caps & MALLOC_CAP_SPIRAM ? psramFree : internalFree;}
size_t heap_caps_get_largest_free_block(int caps){return caps & MALLOC_CAP_SPIRAM ? psramLargest : internalLargest;}
size_t heap_caps_get_total_size(int){return psramTotal;}
DEFINITIONS
int main(){
    DisplayConfigParams config; std::strcpy(config.backend,"rgb"); String error;
    assert(screenFramebufferBytes(&config)==460800);
    assert(screenMemoryAvailable(&config,error));
    psramTotal=0; internalFree=2*1024*1024; internalLargest=1024*1024;
    assert(!screenMemoryAvailable(&config,error) && error=="SCREEN_RGB_PSRAM_REQUIRED");
    psramTotal=8*1024*1024; psramLargest=460799;
    assert(!screenMemoryAvailable(&config,error));
    psramLargest=psramFree=460800+sizeof(ScreenPanelSnapshot)+480*16*2+SCREEN_PSRAM_RESERVE-1;
    assert(!screenMemoryAvailable(&config,error));
    ++psramLargest; ++psramFree; assert(screenMemoryAvailable(&config,error));
    internalFree=SCREEN_INTERNAL_RESERVE_PSRAM-1;
    assert(!screenMemoryAvailable(&config,error));
    internalFree=SCREEN_INTERNAL_RESERVE_PSRAM+TASK_STACK_BYTES-1;
    assert(!screenMemoryAvailable(&config,error));
    ++internalFree; assert(screenMemoryAvailable(&config,error));
    internalFree=2*1024*1024; std::strcpy(config.backend,"spi_tft");psramTotal=0;
    assert(screenFramebufferBytes(&config)==0 && screenMemoryAvailable(&config,error));
    assert(!screenMemoryAvailable(nullptr,error));
}
'''.replace("DEFINITIONS", "\n".join(definitions))
        with tempfile.TemporaryDirectory(prefix="fastbee-screen-memory-") as tmp:
            cpp = pathlib.Path(tmp) / "memory.cpp"
            executable = pathlib.Path(tmp) / ("memory.exe" if os.name == "nt" else "memory")
            cpp.write_text(source, encoding="utf-8")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", str(cpp), "-o", str(executable)],
                                    capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_page_rotation_geometry_and_hit_boundaries(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required")
        with tempfile.TemporaryDirectory(prefix="fastbee-screen-geometry-") as tmp:
            executable = pathlib.Path(tmp) / (
                "screen-geometry.exe" if os.name == "nt" else "screen-geometry"
            )
            compiled = subprocess.run(
                [compiler, "-std=c++17", f"-I{ROOT / 'include'}",
                 str(GEOMETRY_HARNESS), "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True, encoding="utf-8",
                errors="replace", timeout=30,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            executed = subprocess.run(
                [str(executable)], cwd=ROOT, capture_output=True, text=True,
                encoding="utf-8", errors="replace", timeout=15,
            )
            self.assertEqual(executed.returncode, 0, executed.stdout + executed.stderr)

    def test_device_ownership_harness(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("A C++17 compiler is required")

        with tempfile.TemporaryDirectory(prefix="fastbee-screen-device-") as tmp:
            executable = pathlib.Path(tmp) / (
                "screen-device.exe" if os.name == "nt" else "screen-device"
            )
            command = [
                compiler,
                "-std=c++17",
                "-DUNIT_TEST",
                "-DFASTBEE_ENABLE_SCREEN_PANEL=1",
                "-DFASTBEE_ENABLE_SCREEN_UI=1",
                "-include",
                "math.h",
                f"-I{MOCKS}",
                f"-I{ROOT / 'test/unit/cpp/mocks'}",
                f"-I{ROOT / 'test/mocks'}",
                f"-I{ROOT / 'include'}",
                str(HARNESS),
                str(DEVICE_SOURCE),
                "-o",
                str(executable),
            ]
            compiled = subprocess.run(
                command,
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
            self.assertEqual(
                compiled.returncode,
                0,
                f"ScreenDevice harness did not compile:\n{compiled.stdout}\n{compiled.stderr}",
            )
            executed = subprocess.run(
                [str(executable)],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
            self.assertEqual(
                executed.returncode,
                0,
                f"ScreenDevice harness failed:\n{executed.stdout}\n{executed.stderr}",
            )

    def test_teardown_waits_for_ui_task_before_unconfigure(self):
        device = DEVICE_SOURCE.read_text(encoding="utf-8")
        runtime_header = RUNTIME_HEADER.read_text(encoding="utf-8")
        runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
        peripheral = PERIPHERAL_SOURCE.read_text(encoding="utf-8")
        framework = FRAMEWORK_SOURCE.read_text(encoding="utf-8")

        unconfigure = braced_block(
            device, "bool ScreenDevice::unconfigure(const String& peripheralId)"
        )
        self.assertIn("if (_active || _display || _touch) return false;", unconfigure)
        self.assertNotIn("deactivate();", unconfigure)
        self.assertIn("bool stop(String* error = nullptr);", runtime_header)

        stop = braced_block(runtime, "bool ScreenRuntime::stop(String* error)")
        self.assertIn("state.stopRequested.store(true", stop)
        self.assertIn("SCREEN_RUNTIME_STOP_TIMEOUT", stop)
        self.assertRegex(stop, re.compile(r"if \(task\).*?return false;", re.DOTALL))

        teardown = braced_block(
            peripheral, "bool PeripheralManager::teardownHardware(const PeripheralConfig& config)"
        )
        stop_at = teardown.index("ScreenRuntime::instance().stop(&stopError)")
        unconfigure_at = teardown.index("device.unconfigure(config.id)")
        self.assertLess(stop_at, unconfigure_at)
        self.assertIn("SCREEN_DEVICE_STILL_ACTIVE", teardown)

        shutdown = braced_block(framework, "void FastBeeFramework::shutdown()")
        self.assertIn("ScreenRuntime::instance().stop(&screenStopError)", shutdown)

    def test_system_actions_are_queued_and_buzzer_shutdown_is_fail_safe(self):
        runtime_header = RUNTIME_HEADER.read_text(encoding="utf-8")
        runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
        backend = (ROOT / "src/screen/RgbDisplayBackend.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("requestSystemAction(ScreenSystemAction action", runtime_header)
        request = braced_block(
            runtime,
            "bool ScreenRuntime::requestSystemAction(ScreenSystemAction action",
        )
        self.assertIn("state.pendingSystemAction", request)
        self.assertIn("xTaskNotifyGive(state.task)", request)
        self.assertNotIn("restartNetwork", request)
        self.assertIn("BUZZER_PULSE_MS = 80", runtime)
        self.assertIn("BUZZER_OFF_RETRY_MS = 100", runtime)
        self.assertIn("buzzer off write failed; retrying", runtime)
        self.assertIn("for (uint8_t attempt = 0; attempt < 3; ++attempt)", runtime)
        self.assertIn("setBuzzer(false)", backend)
        self.assertIn("updateRegister(outputRegister(), _config.controlBuzzerMask", backend)

    def test_buzzer_mask_is_persisted_routed_and_defaults_to_disabled(self):
        header = PERIPHERAL_HEADER.read_text(encoding="utf-8")
        persistence = PERIPHERAL_PERSISTENCE.read_text(encoding="utf-8")
        route = PERIPHERAL_ROUTE.read_text(encoding="utf-8")

        self.assertIn("uint8_t controlBuzzerMask", header)
        defaults = braced_block(
            header, "inline void initializeDisplayDefaults(PeripheralConfig& config)"
        )
        self.assertIn("config.params.display.controlBuzzerMask = 0", defaults)
        self.assertIn(
            'FastBee::setFixedJsonField(params, "controlBuzzerMask", display.controlBuzzerMask)', persistence
        )
        self.assertIn("LOAD_DISPLAY_EXTRA(controlBuzzerMask, 0, 255)", persistence)
        route_serializer = PERIPHERAL_ROUTE_MAPPER.read_text(encoding="utf-8")
        route_params = PERIPHERAL_PARAM_APPLY.read_text(encoding="utf-8")
        self.assertIn(
            'FastBee::setFixedJsonField(params, "controlBuzzerMask", display.controlBuzzerMask)', route_serializer
        )
        self.assertIn("READ_DISPLAY_EXTRA(controlBuzzerMask, 0, 255)", route_params)
        self.assertIn(
            'controlBuzzerMask && control != "ch32v003"', route_params
        )

    def test_saved_layout_is_refreshed_but_display_change_requires_restart(self):
        panel = PANEL_SOURCE.read_text(encoding="utf-8")
        save = braced_block(panel, "ScreenPanelManager::SaveResult ScreenPanelManager::saveConfig(")
        self.assertIn("displayChanged = _displayRef != nextDisplayRef", save)
        self.assertIn("runtime.stop(&stopError)", save)
        self.assertIn("SCREEN_DISPLAY_RESTART_REQUIRED", save)
        self.assertIn("runtime.requestRefresh()", save)
        self.assertIn("reportApplication(true, false)", save)

    def test_resource_metadata_covers_snapshot_stripe_and_stack(self):
        feature = FEATURE_SOURCE.read_text(encoding="utf-8")
        framework = FRAMEWORK_SOURCE.read_text(encoding="utf-8")
        self.assertRegex(feature, re.compile(
            r'registerFeature\(\{"screenPanel".*?80,\s*69632,.*?nullptr,\s*20,\s*false\}\);',
            re.DOTALL,
        ))
        self.assertIn('{"screenPanel", 20}', framework)
        self.assertIn("screenMemoryAvailable", RUNTIME_SOURCE.read_text(encoding="utf-8"))
        self.assertIn("MALLOC_CAP_SPIRAM", RUNTIME_SOURCE.read_text(encoding="utf-8"))

    def test_renderer_exposes_rotation_icons_and_restart_warning(self):
        runtime = (RUNTIME_SOURCE.read_text(encoding="utf-8") + "\n"

                   + (ROOT / "src/screen/StripeRenderer.h").read_text(encoding="utf-8"))
        self.assertIn("mapScreenRect(", runtime)
        self.assertIn("screenRectContains(rect, x, y)", runtime)
        self.assertNotIn("card.forecastCount", runtime)
        for icon in ("temperature", "humidity", "light", "power", "switch",
                     "wifi", "home", "chart", "clock", "warning"):
            self.assertIn(f'id == "{icon}"', runtime)
        for condition in ("clear-night", "thunder", "fog", "wind", "cold", "hot"):
            self.assertIn(f'"{condition}"', runtime)
        self.assertIn('"历史保存失败"', runtime)
        self.assertIn('"仍不保存并重启?"', runtime)
        self.assertIn('"仍重启"', runtime)
        self.assertIn('"确认重启设备?"', runtime)

    def test_spi_controller_dimensions_are_rejected_at_both_layers(self):
        peripheral = (ROOT / "src/core/PeripheralManagerValidation.cpp").read_text(encoding="utf-8")
        spi = SPI_SOURCE.read_text(encoding="utf-8")
        self.assertIn("shortSide > 240 || longSide > 320", peripheral)
        self.assertIn("DISPLAY_DIMENSIONS_UNSUPPORTED", peripheral)
        self.assertIn("shortSide > 240 || longSide > 320", spi)
        self.assertIn("SCREEN_DISPLAY_DIMENSIONS_UNSUPPORTED", spi)

    def test_spi_and_gt911_backends_keep_shared_bus_and_input_contracts(self):
        spi = SPI_SOURCE.read_text(encoding="utf-8")
        touch = TOUCH_SOURCE.read_text(encoding="utf-8")
        self.assertIn("Do not call SPI.end()", spi)
        self.assertIn("pixelCount < static_cast<size_t>(width) * height", spi)
        self.assertIn("beginTransaction();", spi)
        self.assertIn("endTransaction();", spi)
        self.assertIn("GT911_ADDRESS_PRIMARY = 0x5D", touch)
        self.assertIn("GT911_ADDRESS_SECONDARY = 0x14", touch)
        self.assertIn("auto& bus = I2CBusManager::instance()", touch)
        self.assertIn("_leased = bus.acquire(this, config.touchSda, config.touchScl,", touch)
        self.assertIn("_alternateLeased = bus.acquire(&_alternateOwner, config.touchSda,", touch)
        self.assertIn("I2CBusManager::instance().deferRelease", touch)
        self.assertIn("if (!clearStatus())", touch)
        for rotation in ("case 1:", "case 2:", "case 3:"):
            self.assertIn(rotation, touch)

    def test_render_and_touch_faults_drive_a_bounded_onscreen_indicator(self):
        runtime = RUNTIME_SOURCE.read_text(encoding="utf-8")
        device = DEVICE_SOURCE.read_text(encoding="utf-8")
        header = (ROOT / "src/screen/StripeRenderer.h").read_text(encoding="utf-8")
        self.assertIn("renderFaultFirstMs = now;", runtime)
        self.assertIn("now - renderFaultFirstMs > 5000U", runtime)
        self.assertIn("device.touchError().length() > 0", runtime)
        self.assertIn("renderer.render(*snapshot, confirmationCardId, error, faultFlags)", runtime)
        self.assertIn("renderedFaultFlags = faultFlags;", runtime)
        self.assertIn("if (faultFlags != renderedFaultFlags) forceRender = true;", runtime)
        self.assertIn('_touchError = "SCREEN_TOUCH_COMMUNICATION_FAILED";', device)
        self.assertIn("bool ScreenDevice::serviceTouch(uint32_t nowMs)", device)
        self.assertIn("drawFaultIndicator", header)
        self.assertIn("SCREEN_FAULT_RENDER_RETRY", header)
        self.assertIn("SCREEN_FAULT_TOUCH", header)
        self.assertIn('labels[count++] = "渲染异常";', header)
        self.assertIn('labels[count++] = "触摸异常";', header)


if __name__ == "__main__":
    unittest.main()
