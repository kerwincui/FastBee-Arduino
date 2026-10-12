"""Real passive callback/guard/provider with host RTOS and Wire boundaries."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_io_expander_manager import ROOT, SOURCE, function


class IoExpanderObservationTests(unittest.TestCase):
    def test_real_guard_is_nonblocking_and_never_performs_bus_io(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        guard = (ROOT / 'include/core/AsyncExecTypes.h').read_text(encoding='utf-8')
        start = SOURCE.index('constexpr size_t IO_EXPANDER_MAX_PROVIDERS')
        end = SOURCE.index('std::map<String, uint16_t>& ioPendingSyncs()')
        snippets = [function('class RecursiveMutexGuard', guard) + ';',
                    SOURCE[start:end], function('FastBee::FeatureInstanceSnapshot observeIoExpanderInstances(')]
        with tempfile.TemporaryDirectory(prefix='fastbee-io-observation-') as directory:
            temporary = Path(directory)
            (temporary / 'io_observation_under_test.inc').write_text('\n'.join(snippets), encoding='utf-8')
            binary = temporary / ('observe.exe' if os.name == 'nt' else 'observe')
            command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                       '-Wno-unused-const-variable', f'-I{temporary}',
                       f"-I{ROOT / 'test/unit/cpp/io_expander_provider_mocks'}",
                       f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                       str(ROOT / 'test/unit/cpp/io_expander_observation_harness.cpp'),
                       str(ROOT / 'src/peripherals/IoExpander.cpp'),
                       str(ROOT / 'src/peripherals/IoExpanderProvider.cpp'),
                       str(ROOT / 'src/core/I2CBusManager.cpp'), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
