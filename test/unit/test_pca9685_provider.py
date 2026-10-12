"""Prepared real PCA provider/device/session/bus regressions, with fake OE/Wire."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PCA9685ProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / p for p in (
            'include/peripherals/PCA9685Provider.h',
            'include/core/I2CBusManager.h', 'src/core/I2CBusManager.cpp',
            'include/peripherals/drivers/PCA9685Device.h',
            'include/peripherals/drivers/PCA9685OutputSession.h',
            'include/peripherals/drivers/PCA9685Contract.h',
            'test/unit/cpp/pca9685_provider_harness.cpp',
            'test/unit/test_pca9685_provider.py', 'test/mocks/Arduino.h')]
        for folder in ('pca9685_device_mocks', 'mcp23017_device_mocks/freertos'):
            cls.inputs += sorted(p for p in (ROOT / 'test/unit/cpp' / folder).rglob('*') if p.is_file())
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-pca-provider-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / ('provider.exe' if os.name == 'nt' else 'provider')
        result = subprocess.run([
            compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/unit/cpp/pca9685_device_mocks'),
            '-I' + str(ROOT / 'test/unit/cpp/mcp23017_device_mocks'),
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            str(ROOT / 'test/unit/cpp/pca9685_provider_harness.cpp'),
            str(ROOT / 'src/core/I2CBusManager.cpp'), '-o', str(cls.binary),
        ], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Provider inputs changed during tests')

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(case + ' PASS', result.stdout)


for _case in ('admission', 'normal', 'conversions', 'gate_lost', 'pin_changed', 'busy',
              'begin_busy', 'release_retry', 'stop_failure', 'wake_timeout', 'unexpected_gate',
              'wrap', 'drift', 'transport', 'applied_nack', 'begin_failure',
              'alias_collision', 'enable_failure', 'changed_destructor'):
    setattr(PCA9685ProviderTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
