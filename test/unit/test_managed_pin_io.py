"""Compile actual checked GPIO/DAC implementation with error-valued IDF mocks.

Four chip branches; these are local SDK substitution tests, not electrical tests.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CPP = ROOT / 'test/unit/cpp'


class ManagedPinIoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-managed-pin-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        for chip in ('ESP32', 'ESP32S3', 'ESP32C3', 'ESP32C6'):
            executable = Path(cls.temp.name) / (chip + ('.exe' if os.name == 'nt' else ''))
            build = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-DCONFIG_IDF_TARGET_' + chip + '=1',
                '-I' + str(CPP / 'managed_pin_mocks'), '-I' + str(ROOT / 'include'),
                str(CPP / 'managed_pin_io_harness.cpp'), str(ROOT / 'src/core/ManagedPinIo.cpp'),
                '-o', str(executable)], capture_output=True, text=True, timeout=90)
            if build.returncode:
                raise AssertionError(chip + '\n' + build.stdout + build.stderr)
            cls.binaries[chip] = executable

    def check(self, scenario):
        for chip, executable in self.binaries.items():
            with self.subTest(chip=chip):
                run = subprocess.run([str(executable), scenario], capture_output=True, text=True, timeout=15)
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


for _case in ('lazy_empty', 'lazy_oom', 'lazy_reuse', 'lazy_retained',
              'admission', 'capacity', 'gpio_lifecycle', 'setup_level_failure',
              'inactive_high_release', 'inactive_high_release_failure',
              'setup_config_failure', 'write_failure', 'release_level_failure',
              'release_config_failure', 'foreign', 'irq', 'passive',
              'irq_install_failure', 'irq_type_failure', 'irq_add_failure',
              'irq_disable_failure', 'irq_remove_failure', 'irq_clear_failure',
              'dac_supported', 'dac_new_failure', 'dac_partial_new_failure',
              'dac_write_failure', 'dac_zero_failure', 'dac_delete_failure'):
    setattr(ManagedPinIoTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
