"""Compile the complete production SD manager with checked SPI leases."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdSpiLifecycleTests(unittest.TestCase):
    def test_production_sd_failure_recovery_and_locked_io(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-sd-spi-') as tmp:
            binary = Path(tmp) / ('sd-tests.exe' if os.name == 'nt' else 'sd-tests')
            includes = ['test/unit/cpp/sd_bus_mocks', 'test/unit/cpp/spi_bus_mocks',
                        'test/mocks', 'include']
            sources = ['test/unit/cpp/sd_spi_lifecycle_harness.cpp',
                       'src/peripherals/SDCardManager.cpp', 'src/core/SpiBusInitializer.cpp']
            command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                       *['-I' + str(ROOT / name) for name in includes],
                       *[str(ROOT / name) for name in sources], '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True,
                                    encoding='utf-8', errors='replace', timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
