"""Run the production SPI teardown policy; not a lease or hardware test."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SpiTeardownPolicyTests(unittest.TestCase):
    def test_production_policy_behavior(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-spi-teardown-') as temp:
            binary = Path(temp) / ('spi-tests.exe' if os.name == 'nt' else 'spi-tests')
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-DUNIT_TEST',
                       '-I' + str(ROOT / 'test/unit/cpp/mocks'), '-I' + str(ROOT / 'test/mocks'),
                       '-I' + str(ROOT / 'include'),
                       str(ROOT / 'test/unit/cpp/spi_teardown_policy_harness.cpp'), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_manager_uses_policy_for_bus_and_cs(self):
        source = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        teardown = source[source.index('bool PeripheralManager::teardownHardware('):]
        teardown = teardown[teardown.index('// SPI 总线清理'):teardown.index('// LCD/OLED 显示屏清理')]
        self.assertIn('return teardownSpiHardware(config);', teardown)
        teardown = (ROOT / 'src/core/PeripheralManagerSPI.cpp').read_text(encoding='utf-8')
        teardown = teardown[teardown.index('bool PeripheralManager::teardownSpiHardware('):]
        self.assertIn('protection.include(config, pair.second)', teardown)
        release = 'if (!bus.releaseNamed(config.id, error, config.pins[3], &released)) return fail(error);'
        self.assertIn(release, teardown)
        self.assertLess(teardown.index(release), teardown.index('pinMode(cs, INPUT)'))
        self.assertIn('released && !protection.retainChipSelect && !bus.isChipSelectAcquired(cs)', teardown)
        self.assertNotIn('SPI.end(', teardown)
        self.assertNotIn('bus.end(', teardown)
        self.assertNotIn('SpiBusInitializer::instance().end(', teardown)
        self.assertNotIn('sharedBusActive', teardown)


if __name__ == '__main__':
    unittest.main()
