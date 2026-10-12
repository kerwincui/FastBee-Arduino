"""Real monotonic NC01 controller/config headers; no GPIO or device calls."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ThermostatControllerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-thermostat-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.executables = {}
        for target in ('controller', 'config'):
            executable = Path(cls.temp.name) / (target + '.exe')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-Wno-deprecated-declarations', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                '-DARDUINOJSON_ENABLE_STD_STRING=1', '-I' + str(ROOT / 'test/mocks'),
                '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
                str(ROOT / ('test/unit/cpp/thermostat_' + target + '_harness.cpp')),
                '-o', str(executable)], capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
            cls.executables[target] = executable

    def check(self, target, mode):
        result = subprocess.run([str(self.executables[target]), mode],
            capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('startup', 'heating', 'cooling', 'dwell', 'manual-fault', 'fresh-recovery',
              'stale-future-nan', 'permanent-fault', 'monotonic-64bit'):
    setattr(ThermostatControllerTests, 'test_' + _mode.replace('-', '_'),
            lambda self, mode=_mode: self.check('controller', mode))
for _mode in ('config', 'ranges', 'policy', 'standalone', 'metadata'):
    setattr(ThermostatControllerTests, 'test_' + _mode,
            lambda self, mode=_mode: self.check('config', mode))
