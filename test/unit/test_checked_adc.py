"""Real checked ADC lifecycle with IDF and RTOS doubles; no device access."""
from pathlib import Path
import hashlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PROFILES = {
    'esp32': ['-DADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED=0', '-DADC_CALI_SCHEME_LINE_FITTING_SUPPORTED=1'],
    'c3': ['-DADC_TEST_CHANNELS=5', '-DADC_TEST_C3=1'],
    'c6': ['-DADC_TEST_UNITS=1', '-DADC_TEST_CHANNELS=7', '-DSOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED=1'],
    's3': [],
    'no_calibration': ['-DADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED=0'],
}


def mock_sdk(folder):
    for name in ('Arduino.h', 'freertos/FreeRTOS.h', 'freertos/semphr.h',
                 'esp_adc/adc_oneshot.h', 'esp_adc/adc_cali.h', 'esp_adc/adc_cali_scheme.h', 'soc/soc_caps.h',
                 'esp32-hal-periman.h', 'esp_heap_caps.h'):
        path = folder / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "checked_adc_sdk.h"\n', encoding='utf-8')


class CheckedAdcTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / name for name in ('src/core/CheckedAdc.cpp', 'src/core/AnalogInputCalibration.cpp',
            'include/core/CheckedAdc.h', 'include/core/AdcResourceSnapshot.h', 'include/core/ResourceConsumerIdentity.h',
            'test/unit/cpp/checked_adc_sdk.h', 'test/unit/cpp/checked_adc_harness.cpp.in')]
        cls.hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-checked-adc-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        mock_sdk(folder)
        harness = folder / 'adc.cpp'
        harness.write_text(cls.inputs[-1].read_text(encoding='utf-8'), encoding='utf-8')
        cls.binaries = {}
        for profile, flags in PROFILES.items():
            binary = folder / (profile + '.exe')
            result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                *flags, '-I'+str(folder), '-I'+str(ROOT/'test/unit/cpp'), '-I'+str(ROOT/'include'),
                str(harness), str(cls.inputs[0]), str(cls.inputs[1]), '-o', str(binary)],
                capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise AssertionError(profile + ': ' + result.stdout + result.stderr)
            cls.binaries[profile] = binary

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(path.read_bytes()).hexdigest() for path in cls.inputs]:
            raise AssertionError('Checked ADC inputs changed')

    def check(self, mode):
        for profile, binary in self.binaries.items():
            with self.subTest(profile=profile):
                result = subprocess.run([str(binary), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('cold', 'table_oom', 'table_lifecycle', 'table_transient_lifecycle', 'table_units', 'table_observer',
              'mutex', 'invalid', 'create_failure', 'configure_failure', 'configure_cleanup_failure',
              'zero', 'sample_failure', 'sharing', 'same_channel', 'retained', 'capacity', 'busy', 'calibration',
              'transient', 'transient_retained', 'partial_unit', 'null_unit', 'partial_calibration', 'null_calibration', 'foreign_pin'):
    setattr(CheckedAdcTests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
