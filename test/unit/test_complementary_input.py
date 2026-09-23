"""Run the firmware's real complementary-input state machine on the host."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ComplementaryInputTests(unittest.TestCase):
    def test_fault_episodes_debounce_delivery_and_rollover(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('A C++17 compiler is required')
        with tempfile.TemporaryDirectory(prefix='fastbee-di-pair-') as tmp:
            executable = Path(tmp) / ('pair.exe' if os.name == 'nt' else 'pair')
            result = subprocess.run([
                compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                f'-I{ROOT / "include"}',
                str(ROOT / 'test/unit/cpp/complementary_input_harness.cpp'),
                '-o', str(executable),
            ], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
