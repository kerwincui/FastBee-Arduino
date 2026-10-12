"""Pending final validation: actual UART/I2C publication helpers, no device I/O."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BoundedStreamReadTests(unittest.TestCase):
    def test_bounded_complete_publication_and_failure_preservation(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-stream-read-') as directory:
            binary = Path(directory) / 'read.exe'
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'include'), str(ROOT / 'test/unit/cpp/bounded_stream_read_harness.cpp'),
                '-o', str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

if __name__ == '__main__':
    unittest.main()
