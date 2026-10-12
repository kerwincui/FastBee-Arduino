"""Run production PM expander branches with a real PCF8574 and controllable bus.

This is a focused host harness, not a full firmware/PeripheralManager link. The
unrelated native GPIO, storage and platform code are excluded at explicit branch
boundaries; the expander methods being verified are copied verbatim from source.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "src/core/PeripheralManager.cpp").read_text(encoding="utf-8")


def function(signature, source=SOURCE):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if not depth:
                return source[start:index + 1]
    raise AssertionError(f"Unterminated function: {signature}")


def expander_prefix(signature, boundary, fallback):
    source = function(signature)
    boundaries = boundary if isinstance(boundary, tuple) else (boundary,)
    ending = min(source.index(item) for item in boundaries if item in source)
    return source[:ending] + fallback + "\n}\n"


class IoExpanderManagerTests(unittest.TestCase):
    def test_observer_registers_only_after_creating_the_manager_lock(self):
        initialize = function("bool PeripheralManager::initialize()")
        self.assertLess(initialize.index("xSemaphoreCreateRecursiveMutex()"),
                        initialize.index("registerObserver(&observer)"))
        self.assertIn("if (_mutex)", initialize)
        self.assertIn('ioObservationMutex().store(_mutex, std::memory_order_release)', initialize)
        self.assertIn('"ioExpander", observeIoExpanderInstances, &ioObservationMutex()', initialize)

    def test_production_admission_maintenance_and_blink_branches(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        if not (Path(json_root) / 'ArduinoJson.h').is_file():
            raise RuntimeError('FASTBEE_TEST_ARDUINOJSON_INCLUDE must contain ArduinoJson.h')
        globals_start = SOURCE.index("constexpr size_t IO_EXPANDER_MAX_PROVIDERS")
        globals_end = SOURCE.index("std::map<String, std::unique_ptr<uint8_t>>& i2cOwners()")
        snippets = [SOURCE[globals_start:globals_end]]
        snippets.extend(function(signature) for signature in (
            "bool PeripheralManager::ordinaryIoProducerPresentLocked(",
            "bool PeripheralManager::prepareOrdinaryIoProducerLocked(",
            "void PeripheralManager::discardOrdinaryIoReservationLocked(",
            "bool PeripheralManager::retireOrdinaryIoProducerLocked(",
            "bool PeripheralManager::startOrdinaryIoProducerLocked(",
            "bool PeripheralManager::retireOrdinaryIoGroupLocked(",
            "void PeripheralManager::serviceOrdinaryIoProducerLocked(",
            "bool PeripheralManager::setupIoExpander(",
            "bool PeripheralManager::syncIoExpander(",
            "bool PeripheralManager::teardownIoExpander(",
            "bool PeripheralManager::shutdownIoExpanders(",
            "void PeripheralManager::pollIoExpanders(",
            "bool PeripheralManager::togglePin(const String&",
        ))
        snippets.extend((
            expander_prefix("bool PeripheralManager::validateIoDependencies(",
                            "    if (config.type == PeripheralType::IO_EXPANDER)", "    return true;"),
            expander_prefix("bool PeripheralManager::setupHardware(",
                            "    // 加固：统一校验", "    return false;"),
            expander_prefix("bool PeripheralManager::teardownHardware(",
                            ("    if (config.type == PeripheralType::SCREEN_DISPLAY)",
                             "    if (config.isGPIOPeripheral())"), "    return false;"),
            expander_prefix("GPIOState PeripheralManager::readGpioStateLocked(",
                            "    uint8_t pin = config.getPrimaryPin();", "    return GPIOState::STATE_UNDEFINED;"),
            expander_prefix("bool PeripheralManager::writeGpioStateLocked(",
                            "    uint8_t pin = config.getPrimaryPin();", "    return false;"),
        ))
        rebooter = (ROOT / "src/systems/SystemRebooter.cpp").read_text(encoding="utf-8")
        snippets.append(function("void SystemRebooter::update()", rebooter))
        with tempfile.TemporaryDirectory(prefix="fastbee-io-manager-") as directory:
            temporary = Path(directory)
            (temporary / "io_manager_under_test.inc").write_text("\n\n".join(snippets), encoding="utf-8")
            executable = temporary / ("manager.exe" if os.name == "nt" else "manager")
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-unused-const-variable",
                       f"-I{temporary}", f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       '-I' + json_root,
                       str(ROOT / "test/unit/cpp/io_expander_manager_harness.cpp"),
                       str(ROOT / "src/peripherals/IoExpander.cpp"), "-o", str(executable)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding="utf-8", errors="replace", timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_framework_shutdown_stops_actions_before_expander_safe_state(self):
        framework = (ROOT / "src/core/FastBeeFramework.cpp").read_text(encoding="utf-8")
        shutdown = function("void FastBeeFramework::shutdown()", framework)
        self.assertLess(shutdown.index("stopAllTasks()"), shutdown.index("shutdownIoExpanders()"))
        self.assertIn("IO_SHUTDOWN_UNCONFIRMED", shutdown)
        self.assertLess(shutdown.index("shutdownIoExpanders()"), shutdown.index("network->disconnect()"))


if __name__ == "__main__":
    unittest.main()
