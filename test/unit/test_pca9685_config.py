"""Prepared production PCA config/action parser tests; no device or gate I/O."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Pca9685ConfigTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-pca-config-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / ('config.exe' if os.name == 'nt' else 'config')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'), '-I' + json_root,
            str(ROOT / 'src/core/Pca9685ConfigSupport.cpp'),
            str(ROOT / 'test/unit/cpp/pca9685_config_harness.cpp'), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('roundtrip', 'schema', 'ranges', 'references', 'commands', 'reject_commands'):
    setattr(Pca9685ConfigTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
