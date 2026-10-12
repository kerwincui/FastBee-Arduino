"""Actual PM GPIO/DAC/LEDC methods and checked pin backend; platform substitutes.

ADC's zero-wait busy/retained query is substituted here; CheckedAdc has its own
real backend tests. No result is physical acceptance or global SDK ownership.
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


class ManagedPinManagerTests(unittest.TestCase):
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
        gpio = (ROOT / 'src/core/PeripheralManagerGpio.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(gpio, signature) for signature in (
            'bool PeripheralManager::managedPinConflict(', 'bool PeripheralManager::managedPinAvailableForAdc(',
            'bool PeripheralManager::setupManagedDigital(', 'bool PeripheralManager::releaseManagedDigital(',
            'bool PeripheralManager::writeManagedDac(', 'bool PeripheralManager::releaseManagedDac(',
            'bool PeripheralManager::prepareScriptPin(', 'bool PeripheralManager::scriptPinReady(',
            'bool PeripheralManager::writeScriptGpio(', 'bool PeripheralManager::writeScriptDac(',
            'void PeripheralManager::snapshotGpioResources(', 'void PeripheralManager::snapshotDacResources('))
        ledc = (ROOT / 'src/core/PeripheralManagerLedc.cpp').read_text(encoding='utf-8')
        methods += '\n' + '\n'.join(definition(ledc, signature) for signature in (
            'bool PeripheralManager::attachManagedLedc(', 'bool PeripheralManager::detachManagedLedc(',
            'bool PeripheralManager::writeScriptPwm(', 'bool PeripheralManager::releaseScriptPwm(',
            'void PeripheralManager::snapshotLedcResources('))
        template = (CPP / 'managed_pin_manager_harness.cpp.in').read_text(encoding='utf-8')
        template = template.replace('// @rf_mode@', rf_mode)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-managed-pin-manager-')
        cls.addClassCleanup(cls.temp.cleanup)
        source = Path(cls.temp.name) / 'manager.cpp'
        source.write_text(template.replace('// @production@', methods), encoding='utf-8')
        cls.binaries = {}
        for chip in ('ESP32', 'ESP32S3', 'ESP32C3', 'ESP32C6'):
            executable = Path(cls.temp.name) / (chip + ('.exe' if os.name == 'nt' else ''))
            build = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-DCONFIG_IDF_TARGET_' + chip + '=1', '-I' + str(CPP),
                '-I' + str(CPP / 'managed_pin_mocks'), '-I' + str(ROOT / 'include'),
                '-I' + json_root,
                str(source), str(ROOT / 'src/core/ManagedPinIo.cpp'), '-o', str(executable)],
                capture_output=True, text=True, timeout=90)
            if build.returncode:
                raise AssertionError(chip + '\n' + build.stdout + build.stderr)
            cls.binaries[chip] = executable

    def check(self, scenario):
        for chip, executable in self.binaries.items():
            with self.subTest(chip=chip):
                run = subprocess.run([str(executable), scenario], capture_output=True, text=True, timeout=15)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


for _case in ('switch', 'dac_switch', 'native', 'adc_overlap', 'gpio_types', 'admission', 'passive_busy'):
    setattr(ManagedPinManagerTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
