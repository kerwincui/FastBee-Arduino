"""Compile complete production TFT and lease manager against hardware mocks."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class TftSpiLifecycleTests(unittest.TestCase):
    def test_production_tft_failure_sharing_stop_and_repeated_cycles(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-tft-lease-') as tmp:
            binary = Path(tmp) / ('tft.exe' if os.name == 'nt' else 'tft')
            command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                       '-DUNIT_TEST', '-DARDUINO_ARCH_ESP32', '-DFASTBEE_ENABLE_SCREEN_UI=1',
                       '-include', 'math.h']
            command += ['-I' + str(ROOT / path) for path in (
                'test/unit/cpp/tft_bus_mocks', 'test/unit/cpp/spi_bus_mocks',
                'test/unit/cpp/mocks', 'test/mocks', 'include')]
            command += [str(ROOT / path) for path in (
                'test/unit/cpp/tft_spi_lifecycle_harness.cpp',
                'src/screen/SpiTftDisplayBackend.cpp', 'src/core/SpiBusInitializer.cpp')]
            command += ['-o', str(binary)]
            built = subprocess.run(command, capture_output=True, text=True,
                                   encoding='utf-8', errors='replace', timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True,
                                 encoding='utf-8', errors='replace', timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

if __name__ == '__main__':
    unittest.main()
