"""PCA session fault cases prepared for the final consolidated test pass."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PCA9685SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / path for path in ('include/peripherals/drivers/PCA9685OutputSession.h',
                      'include/peripherals/drivers/PCA9685Contract.h', 'test/unit/cpp/pca9685_session_harness.cpp')]
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-pca9685-session-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.binary = Path(cls.directory.name) / ('session.exe' if os.name == 'nt' else 'session')
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'include'), str(cls.inputs[-1]), '-o', str(cls.binary)],
            capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Session inputs changed during tests')

    def check(self, case):
        ran = subprocess.run([str(self.binary), case], capture_output=True, text=True, timeout=10)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
        self.assertIn(case + ' PASS', ran.stdout)


for _case in ('preconditions', 'wake', 'timeout', 'reset', 'init_faults', 'update_faults',
              'channel', 'drift', 'enable_and_stop', 'gate'):
    setattr(PCA9685SessionTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
