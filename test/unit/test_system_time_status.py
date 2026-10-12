"""Shared device/batch time serializer uses one source epoch and guarded TZ."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SystemTimeStatusTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            installed = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not installed:
                raise RuntimeError('Installed ArduinoJson required')
            json_root = str(installed[0].parent)
        cls.folder = tempfile.TemporaryDirectory(prefix='fastbee-time-status-')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.binary = Path(cls.folder.name) / 'status.exe'
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'), '-I'+json_root,
            str(ROOT/'src/utils/SystemTimeZone.cpp'), str(ROOT/'test/unit/cpp/system_time_status_harness.cpp'),
            str(ROOT/'src/network/FixedJsonField.cpp'),
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary),case], capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


for _case in ('valid','invalid','source_busy','timezone_busy','format_failed','unknown','rtc'):
    setattr(SystemTimeStatusTests,'test_'+_case,lambda self,case=_case:self.check(case))

if __name__ == '__main__':
    unittest.main()
