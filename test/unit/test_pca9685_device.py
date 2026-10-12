"""Prepared PCA9685 adapter/real manager tests. No devices or host clock writes."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PCA9685DeviceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / p for p in (
            'include/core/I2CBusManager.h', 'src/core/I2CBusManager.cpp',
            'include/peripherals/drivers/PCA9685Device.h',
            'include/peripherals/drivers/PCA9685OutputSession.h',
            'include/peripherals/drivers/PCA9685Contract.h',
            'test/unit/cpp/pca9685_device_harness.cpp', 'test/mocks/Arduino.h')]
        for folder in ('pca9685_device_mocks', 'mcp23017_device_mocks/freertos'):
            cls.inputs += sorted(p for p in (ROOT / 'test/unit/cpp' / folder).rglob('*') if p.is_file())
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-pca9685-device-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / ('device.exe' if os.name == 'nt' else 'device')
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/unit/cpp/pca9685_device_mocks'),
            '-I'+str(ROOT/'test/unit/cpp/mcp23017_device_mocks'),
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'),
            str(ROOT/'test/unit/cpp/pca9685_device_harness.cpp'),
            str(ROOT/'src/core/I2CBusManager.cpp'), '-o', str(cls.exe)],
            capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Device inputs changed during tests')

    def check(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + ' PASS', result.stdout)


for _case in ('admission', 'main_collision', 'alias_collision', 'normal', 'pointer_short',
              'write_short', 'nack', 'short_read', 'negative', 'wide', 'empty', 'extra',
              'infinite', 'lease_lost', 'busy', 'deferred', 'capacity', 'gate_failure', 'init_failure',
              'gate_unowned','gate_sda','gate_scl','gate_invalid','gate_lost',
              'gate_changed','ended_destructor'):
    setattr(PCA9685DeviceTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
