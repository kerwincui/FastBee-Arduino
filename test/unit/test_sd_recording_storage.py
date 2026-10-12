"""Compile the complete production SD manager and recorder transaction, no devices."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdRecordingStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-sd-record-storage-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / ('storage.exe' if os.name == 'nt' else 'storage')
        includes = ['test/unit/cpp/sd_recording_mocks', 'test/unit/cpp/sd_bus_mocks',
                    'test/unit/cpp/spi_bus_mocks', 'test/mocks', 'include']
        sources = ['test/unit/cpp/sd_recording_storage_harness.cpp', 'src/peripherals/SDCardManager.cpp',
                   'src/peripherals/SDCardRecording.cpp', 'src/core/SpiBusInitializer.cpp']
        built = subprocess.run([compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            *['-I' + str(ROOT / path) for path in includes], *[str(ROOT / path) for path in sources],
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _mode in ('owner', 'generation', 'recovery', 'lease', 'busy', 'short', 'flush', 'collision',
              'changed', 'reserved', 'rotation', 'rotate-failure', 'bounds',
              'exists-false', 'create-race', 'rotate-race'):
    setattr(SdRecordingStorageTests, 'test_' + _mode.replace('-', '_'), lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
