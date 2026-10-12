"""Actual encoder setup/teardown/ISR/read methods and ManagedPinIo, SDK substituted.

This proves local failure propagation and stale-callback isolation, not physical
encoder frequency, wiring, GPIO electrical state, or complete SDK ownership.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'


class EncoderCheckedLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        if not (Path(json_root) / 'ArduinoJson.h').is_file():
            raise RuntimeError('FASTBEE_TEST_ARDUINOJSON_INCLUDE must contain ArduinoJson.h')
        config = (ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf-8')
        rf_mode = next(line for line in config.splitlines() if line.startswith('constexpr uint8_t RF_MODE_TX ='))
        header = (ROOT / 'include/core/PeripheralManager.h').read_text(encoding='utf-8')
        io = (ROOT / 'src/core/PeripheralManagerIo.cpp').read_text(encoding='utf-8')
        encoder = (ROOT / 'src/core/PeripheralManagerEncoder.cpp').read_text(encoding='utf-8')
        gpio = (ROOT / 'src/core/PeripheralManagerGpio.cpp').read_text(encoding='utf-8')
        manager = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(encoder, signature) for signature in (
            'bool PeripheralManager::encoderSlotReady(',
            'bool PeripheralManager::setupEncoderHardware(',
            'bool PeripheralManager::teardownEncoderHardware('))
        methods += '\n' + '\n'.join(definition(io, signature) for signature in (
            'bool PeripheralManager::getEncoderReading(', 'bool PeripheralManager::resetEncoder(',
            'void PeripheralManager::snapshotEncoderResources(',
            'PeripheralManager::EncoderIsrSlot* PeripheralManager::findEncoderSlot(',
            'PeripheralManager::EncoderIsrSlot* PeripheralManager::allocEncoderSlot(',
            'void PeripheralManager::releaseEncoderSlot(',
            'void IRAM_ATTR PeripheralManager::handleEncoderInterrupt('))
        methods += '\n' + definition(gpio, 'bool PeripheralManager::managedPinConflict(')
        read = definition(manager, 'GPIOState PeripheralManager::readPin(const String&')
        branch = definition(read, 'if (config->type == PeripheralType::ENCODER)')
        template = (CPP / 'encoder_checked_lifecycle_harness.cpp.in').read_text(encoding='utf-8')
        template = template.replace('// @rf_mode@', rf_mode)
        template = template.replace('// @slot@', definition(header, 'struct EncoderIsrSlot') + ';')
        template = template.replace('// @read_branch@', branch)
        template = template.replace('// @production@', methods)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-encoder-checked-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'encoder.cpp'
        source.write_text(template, encoding='utf-8')
        cls.binaries = {}
        for chip in ('ESP32', 'ESP32S3', 'ESP32C3', 'ESP32C6'):
            binary = Path(cls.temp.name) / (chip + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-DCONFIG_IDF_TARGET_' + chip + '=1', '-I' + str(CPP / 'managed_pin_mocks'),
                '-I' + str(CPP), '-I' + str(ROOT / 'include'), '-I' + json_root, str(source),
                str(ROOT / 'src/core/ManagedPinIo.cpp'), '-o', str(binary)],
                capture_output=True, text=True, timeout=90)
            if built.returncode:
                raise AssertionError(chip + '\n' + built.stdout + built.stderr)
            cls.binaries[chip] = binary

    def check(self, scenario):
        for chip, binary in self.binaries.items():
            with self.subTest(chip=chip):
                ran = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=15)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_manager_dispatches_real_helpers_and_readiness(self):
        source = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        self.assertIn('return setupEncoderHardware(config);', definition(source, 'bool PeripheralManager::setupHardware('))
        teardown = definition(source, 'bool PeripheralManager::teardownHardware(')
        self.assertIn('return teardownEncoderHardware(config);', teardown)
        self.assertNotIn('releaseEncoderSlot(', teardown)


for _case in ('healthy', 'add_failure', 'remove_failure', 'disable_failure',
              'type_cleanup_failure', 'setup_a_failure', 'setup_b_failure',
              'pad_cleanup_failure', 'capacity', 'generation_exhaustion',
              'admission', 'busy_snapshot', 'concurrent_retirement', 'counter_saturation'):
    setattr(EncoderCheckedLifecycleTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
