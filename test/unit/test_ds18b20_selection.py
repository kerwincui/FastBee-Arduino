"""Actual saved-rule/executor ROM parser and production Web form, offline only."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Ds18b20SelectionTests(unittest.TestCase):
    def test_actual_parser(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        self.assertTrue(headers)
        with tempfile.TemporaryDirectory(prefix='fastbee-ds18-rom-') as folder:
            binary = Path(folder) / ('rom.exe' if os.name == 'nt' else 'rom')
            run = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'include'), '-I' + str(headers[0].parent),
                str(ROOT / 'test/unit/cpp/ds18b20_selection_harness.cpp'), '-o', str(binary)],
                capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_actual_web_form(self):
        run = subprocess.run(['node', '--test', 'test/unit/test_ds18b20_selection.cjs'],
            cwd=ROOT, capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == '__main__':
    unittest.main()
