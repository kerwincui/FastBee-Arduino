"""Real radar PM methods, sampled filter, SDK-checked GPIO and read consumers.

SDK/clock/config lookup substitutes only; no physical input or timing claims.
The original raw read remains an executable negative, not a weakened oracle.
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


class RadarInputLifecycleTests(unittest.TestCase):
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
        source = (ROOT / 'src/core/PeripheralManagerRadar.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(definition(source, signature) for signature in (
            'bool PeripheralManager::setupRadarHardware(',
            'bool PeripheralManager::teardownRadarHardware(',
            'bool PeripheralManager::readRadarState('))
        manager = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        gpio = (ROOT / 'src/core/PeripheralManagerGpio.cpp').read_text(encoding='utf-8')
        methods += '\n' + definition(gpio, 'bool PeripheralManager::managedPinConflict(')
        config = (ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf-8')
        union = definition(definition(config, 'struct PeripheralRuntimeState'), 'union {')
        old_union = union.replace('        FastBee::RadarInputState radar;\n', '')
        if union == old_union:
            raise AssertionError('Radar state must be the sole addition to the existing runtime union')
        rf_mode = next(line for line in config.splitlines() if line.startswith('constexpr uint8_t RF_MODE_TX ='))
        legacy = (CPP / 'radar_raw_read_oracle.inc').read_text(encoding='utf-8')
        legacy = legacy.replace('::readRadarState(', '::legacyReadRadarState(')
        route = (ROOT / 'src/network/handlers/PeripheralReadRoutes.cpp').read_text(encoding='utf-8')
        executor = (ROOT / 'src/core/PeriphExecExecutor.cpp').read_text(encoding='utf-8')
        io = (ROOT / 'src/core/PeripheralManagerIo.cpp').read_text(encoding='utf-8')
        read_data = io[io.index('case PeripheralType::RADAR_SENSOR:'):io.index('case PeripheralType::RF_MODULE:', io.index('case PeripheralType::RADAR_SENSOR:'))]
        template = (CPP / 'radar_input_lifecycle_harness.cpp.in').read_text(encoding='utf-8')
        replacements = {
            '// @rf_mode@': rf_mode,
            '// @union@': union + ' state;',
            '// @old_union@': old_union + ' state;',
            '// @production@': methods + '\n' + legacy,
            '// @setup@': definition(definition(manager, 'bool PeripheralManager::setupHardware('), 'if (config.type == PeripheralType::RADAR_SENSOR)'),
            '// @teardown@': definition(definition(manager, 'bool PeripheralManager::teardownHardware('), 'if (config.type == PeripheralType::RADAR_SENSOR)'),
            '// @read_pin@': definition(definition(manager, 'GPIOState PeripheralManager::readPin(const String&'), 'if (config->type == PeripheralType::RADAR_SENSOR)'),
            '// @read_data@': read_data,
            '// @route@': definition(route, 'if (config.type == PeripheralType::RADAR_SENSOR)'),
            '// @sensor@': definition(executor, 'if (strcmp(category, "radar") == 0)'),
        }
        for marker, content in replacements.items():
            if template.count(marker) != 1:
                raise AssertionError(marker)
            template = template.replace(marker, content)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-radar-input-')
        cls.addClassCleanup(cls.temp.cleanup)
        compiled = Path(cls.temp.name) / 'radar.cpp'
        compiled.write_text(template, encoding='utf-8')
        cls.binaries = {}
        for chip in ('ESP32', 'ESP32S3', 'ESP32C3', 'ESP32C6'):
            binary = Path(cls.temp.name) / (chip + ('.exe' if os.name == 'nt' else ''))
            built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                '-DCONFIG_IDF_TARGET_' + chip + '=1', '-I' + str(CPP / 'managed_pin_mocks'),
                '-I' + str(CPP), '-I' + str(ROOT / 'include'), '-I' + json_root, str(compiled),
                str(ROOT / 'src/core/ManagedPinIo.cpp'), str(ROOT / 'src/network/FixedJsonField.cpp'),
                '-o', str(binary)], capture_output=True, text=True, timeout=90)
            if built.returncode:
                raise AssertionError(chip + '\n' + built.stdout + built.stderr)
            cls.binaries[chip] = binary

    def check(self, scenario):
        for chip, binary in self.binaries.items():
            with self.subTest(chip=chip):
                ran = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=15)
                self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('initial_low', 'initial_high', 'active_low', 'zero_intervals',
              'both_edges', 'glitch', 'hold', 'hold_glitch', 'hold_retrigger',
              'hold_boundary', 'wrap_debounce', 'wrap_hold', 'bounds', 'admission',
              'setup_failure', 'release_failure', 'owner_loss', 'config_drift',
              'runtime_guards', 'generation', 'rollback_snapshot', 'idempotent_setup',
              'generation_exhaustion', 'busy', 'two_owners', 'read_consumers',
              'legacy_negative', 'union_size'):
    setattr(RadarInputLifecycleTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
