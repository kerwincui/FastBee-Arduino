"""Actual target-baseline/action validator; no Modbus transport or device access."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ConfigMigrationModbusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-migration-modbus-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name)/'modbus-review.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-Wno-deprecated-declarations', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
            '-DARDUINOJSON_ENABLE_STD_STRING=1', '-I'+str(ROOT/'test/mocks'),
            '-I'+str(ROOT/'include'), '-I'+str(headers[0].parent),
            str(ROOT/'test/unit/cpp/config_migration_modbus_harness.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _mode in ('poll', 'shape', 'relay', 'pwm', 'pid-motor', 'disabled', 'baseline'):
    setattr(ConfigMigrationModbusTests, 'test_'+_mode.replace('-', '_'),
            lambda self, mode=_mode: self.check(mode))
