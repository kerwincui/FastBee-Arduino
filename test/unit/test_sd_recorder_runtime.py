"""Whole production recorder runtime plus actual schema/buffer with controlled SDK seams."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdRecorderRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root and headers:
            json_root = str(headers[0].parent)
        if not compiler or not json_root:
            raise RuntimeError('C++17 compiler and installed ArduinoJson required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-sd-recorder-runtime-')
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)
        source = (ROOT / 'src/core/PeriphExecSdRecorder.cpp').read_text(encoding='utf-8')
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.MULTILINE)
        source = source.replace('#if FASTBEE_ENABLE_PERIPH_EXEC\n', '', 1)
        if not source.rstrip().endswith('#endif'):
            raise AssertionError('Production feature wrapper changed')
        source = source.rstrip()[:-len('#endif')]
        (directory / 'sd_recorder_runtime_methods.inc').write_text(source, encoding='utf-8')
        cls.binary = directory / 'runtime.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
            '-I' + str(directory), '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            '-I' + json_root, str(ROOT / 'test/unit/cpp/sd_recorder_runtime_harness.cpp'),
            str(ROOT / 'src/network/FixedJsonField.cpp'),
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _mode in ('config', 'binding', 'queue', 'status', 'normal', 'wall', 'old', 'stale', 'invalid',
              'duplicates', 'write-fault', 'unmount', 'source-change', 'graph-change', 'disable',
              'shutdown', 'memory', 'busy-retry'):
    setattr(SdRecorderRuntimeTests, 'test_' + _mode.replace('-', '_'), lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
