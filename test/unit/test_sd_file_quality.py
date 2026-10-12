"""Prepared whole production SD manager read/recovery/append regressions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdFileQualityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-sd-file-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / ('file.exe' if os.name == 'nt' else 'file')
        includes = ['test/unit/cpp/sd_bus_mocks', 'test/unit/cpp/spi_bus_mocks', 'test/mocks', 'include']
        sources = ['test/unit/cpp/sd_file_quality_harness.cpp',
                   'src/peripherals/SDCardManager.cpp', 'src/core/SpiBusInitializer.cpp']
        result = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            *['-I'+str(ROOT/name) for name in includes], *[str(ROOT/name) for name in sources],
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def check(self, case):
        result = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('normal', 'empty', 'binary', 'short', 'negative', 'wide', 'extra', 'grow',
              'shrink', 'endless', 'oversize', 'append_overflow', 'append_unreadable', 'append_boundary'):
    setattr(SdFileQualityTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
