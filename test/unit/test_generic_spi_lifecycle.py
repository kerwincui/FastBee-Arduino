"""Real SPI initializer/setup/teardown and production SPI I/O branch tests.

Only external Arduino/manager container dependencies are mocks. This does not
claim full PeripheralManager, host-wide W5500, or physical timing coverage.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class GenericSpiLifecycleTests(unittest.TestCase):
    def test_production_named_ownership_and_manager_io(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            self.skipTest('C++17 compiler required')
        source = (ROOT / 'src/core/PeripheralManagerIo.cpp').read_text(encoding='utf-8')
        template = (ROOT / 'test/unit/cpp/generic_spi_lifecycle_harness.cpp.in').read_text(encoding='utf-8')
        for name, next_case in [('writeData', 'RF_MODULE'), ('readData', 'RADAR_SENSOR')]:
            start = source.index('bool PeripheralManager::' + name + '(')
            switch = source.index('switch (config->type)', start)
            branch = source.index('case PeripheralType::SPI:', switch)
            end = source.index('case PeripheralType::' + next_case, branch)
            method = source[start:switch] + 'switch (config->type) {\n'
            method += source[branch:end] + 'default: break;\n}\nreturn success;\n}\n'
            template = template.replace('// @' + name + '@', method)
        self.assertNotIn('// @', template)
        with tempfile.TemporaryDirectory(prefix='fastbee-generic-spi-') as tmp:
            cpp = Path(tmp) / 'generic.cpp'
            cpp.write_text(template, encoding='utf-8')
            binary = Path(tmp) / ('generic.exe' if os.name == 'nt' else 'generic')
            includes = ['test/unit/cpp/generic_spi_mocks', 'test/unit/cpp/spi_bus_mocks',
                        'test/mocks', 'include']
            command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                       *['-I' + str(ROOT / path) for path in includes], str(cpp),
                       str(ROOT / 'src/core/SpiBusInitializer.cpp'),
                       str(ROOT / 'src/core/PeripheralManagerSPI.cpp'), '-o', str(binary)]
            compiled = subprocess.run(command, capture_output=True, text=True,
                                      encoding='utf-8', errors='replace', timeout=60)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_call_chain_and_fixed_capacity_contract(self):
        manager = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        io = (ROOT / 'src/core/PeripheralManagerIo.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/SpiBusInitializer.h').read_text(encoding='utf-8')
        helper = (ROOT / 'src/core/PeripheralManagerSPI.cpp').read_text(encoding='utf-8')
        profile = (ROOT / 'include/core/ResourceProfile.h').read_text(encoding='utf-8')
        self.assertIn('return setupSpiHardware(config);', manager)
        self.assertIn('return teardownSpiHardware(config);', manager)
        self.assertIn('MAX_PERIPHERAL_ID_LEN = 40', profile)
        self.assertIn('MAX_CLIENTS = 16', header)
        self.assertIn('char name[MAX_NAME_LENGTH + 1]', header)
        self.assertNotIn('SPI.end(', helper)
        self.assertNotIn('SpiBusInitializer::instance().end(', helper)
        for method in ['writeData', 'readData']:
            section = io[io.index('bool PeripheralManager::' + method + '('):]
            self.assertLess(section.index('RecursiveMutexGuard lock(_mutex)'),
                            section.index('FastBee::SpiBusOperation operation(20)'))
            self.assertLess(section.index('checkNamed('), section.index('SPI.beginTransaction('))


if __name__ == '__main__':
    unittest.main()
