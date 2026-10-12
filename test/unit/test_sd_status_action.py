"""Prepared for final validation; actual executor with passive driver/RTOS fakes."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdStatusActionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-sd-action-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / 'sd-action.exe'
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DFASTBEE_ENABLE_PERIPH_EXEC=1',
                   '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-I' + json_root]
        command += ['-I' + str(ROOT / path) for path in (
            'test/unit/cpp/sd_action_mocks', 'test/unit/cpp/rtc_action_mocks',
            'test/mocks', 'test/unit/cpp/mocks', 'include')]
        command += [str(ROOT / 'test/unit/cpp/sd_action_harness.cpp'),
                    str(ROOT / 'src/core/PeriphExecStorage.cpp'), '-o', str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('long_age', 'valid', 'busy', 'missing', 'wrong_type', 'disabled', 'suspended', 'unavailable',
              'owner', 'pins', 'frequency', 'value', 'received', 'wrong_action',
              'unmounted', 'capacity_invalid', 'capacity_bounds', 'recovery'):
    setattr(SdStatusActionTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
