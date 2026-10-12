"""Execute real MCP register session against deterministic bus faults, no devices."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MCP23017SessionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT/'include/peripherals/drivers/MCP23017PortSession.h',
                      ROOT/'include/peripherals/drivers/MCP23017Contract.h',
                      ROOT/'test/unit/cpp/mcp23017_session_harness.cpp']
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-mcp23017-session-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name)/('session.exe' if os.name == 'nt' else 'session')
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), str(cls.inputs[-1]), '-o', str(cls.executable)],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Session inputs changed during tests')

    def check(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True,
                                text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn(name+' PASS', result.stdout)


for _case in ('preconditions', 'reset_checks', 'ordering', 'init_failures', 'latch_not_gpio',
              'update_failures', 'sample_failures', 'drift', 'ownership', 'restart', 'preload_readback',
              'config_ordering', 'config_same', 'config_invalid', 'config_failures',
              'config_ownership', 'config_ignored', 'config_drift', 'config_logical_inputs'):
    setattr(MCP23017SessionTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
