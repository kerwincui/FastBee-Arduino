"""Compile pure H1 register contracts; no drivers registered and no devices."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class H1RegisterContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise unittest.SkipTest('C++17 compiler required')
        cls.directory = tempfile.TemporaryDirectory(prefix='fastbee-h1-contracts-')
        cls.addClassCleanup(cls.directory.cleanup)
        cls.executable = Path(cls.directory.name) / ('contracts.exe' if os.name == 'nt' else 'contracts')
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), str(ROOT/'test/unit/cpp/h1_register_contract_harness.cpp'),
            '-o', str(cls.executable)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name+' PASS', result.stdout)

for _case in ('ads_config','ads_reject','ads_decode','pca_frequency','pca_reject','pca_endpoints','pca_pwm'):
    setattr(H1RegisterContractTests, 'test_'+_case, lambda self, case=_case: self.run_case(case))

if __name__ == '__main__':
    unittest.main()
