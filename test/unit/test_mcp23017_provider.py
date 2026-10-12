"""Prepared real provider/session/I2C regressions; native RESET and Wire are fakes."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class MCP23017ProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.inputs = [ROOT / p for p in (
            'include/peripherals/MCP23017Provider.h', 'src/peripherals/MCP23017Provider.cpp',
            'include/peripherals/DigitalIoProvider.h', 'include/peripherals/IoExpander.h',
            'include/peripherals/drivers/MCP23017Device.h',
            'include/peripherals/drivers/MCP23017PortSession.h',
            'include/peripherals/drivers/MCP23017Contract.h',
            'include/core/I2CBusManager.h', 'src/core/I2CBusManager.cpp',
            'test/unit/cpp/mcp23017_provider_harness.cpp')]
        cls.inputs += sorted(p for p in (ROOT / 'test/unit/cpp/mcp23017_device_mocks').rglob('*') if p.is_file())
        cls.inputs += sorted(p for p in (ROOT / 'test/mocks').rglob('*') if p.is_file())
        cls.hashes = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mcp-provider-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / ('provider.exe' if os.name == 'nt' else 'provider')
        result = subprocess.run([
            compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/unit/cpp/mcp23017_device_mocks'),
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            str(ROOT / 'test/unit/cpp/mcp23017_provider_harness.cpp'),
            str(ROOT / 'src/peripherals/MCP23017Provider.cpp'),
            str(ROOT / 'src/core/I2CBusManager.cpp'), '-o', str(cls.exe)],
            capture_output=True, text=True, timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.hashes != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError('Provider inputs changed during tests')

    def check(self, name):
        result = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + ' PASS', result.stdout)


for _case in ('arguments', 'normal', 'freshness', 'wrap', 'configure', 'busy',
              'begin_busy', 'collision', 'reset_not_owned', 'reset_failed', 'reset_lost', 'reset_pin_changed',
              'transport_fault', 'sample_fault', 'drift', 'shutdown_fault', 'shutdown_busy', 'release_retry',
              'destructor', 'cached_reset_lost', 'cached_pin_changed'):
    setattr(MCP23017ProviderTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
