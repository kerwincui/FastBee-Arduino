"""Pure MCP23017 register/mask contracts; no bus access or production registration."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]

class MCP23017ContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.paths = [ROOT / 'include/peripherals/drivers/MCP23017Contract.h',
                     ROOT / 'test/unit/cpp/mcp23017_contract_harness.cpp']
        cls.before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.paths]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mcp-contract-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / ('case.exe' if os.name == 'nt' else 'case')
        result = subprocess.run([compiler, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), str(cls.paths[1]), '-o', str(cls.exe)],
            capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.before != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.paths]:
            raise AssertionError('Inputs changed')

    def check_mode(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout+result.stderr)

for mode in ('registers','directions','reject','latch'):
    setattr(MCP23017ContractTests, 'test_'+mode, lambda self, mode=mode:self.check_mode(mode))

if __name__ == '__main__':
    unittest.main(verbosity=2)
