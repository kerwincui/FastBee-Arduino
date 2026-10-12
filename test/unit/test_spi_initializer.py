"""Real checked-SPI initialization and source-derived driver failure paths."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def compile_and_run(test, sources, internal_heap=False):
    compiler = shutil.which('g++') or shutil.which('clang++')
    if not compiler:
        test.skipTest('C++17 compiler required')
    with tempfile.TemporaryDirectory(prefix='fastbee-spi-init-') as tmp:
        binary = Path(tmp) / ('spi-tests.exe' if os.name == 'nt' else 'spi-tests')
        allocator_flags = []
        if internal_heap:
            (Path(tmp) / 'esp_heap_caps.h').write_text(
                '#pragma once\n#include <cstddef>\n'
                'constexpr unsigned MALLOC_CAP_INTERNAL=1, MALLOC_CAP_8BIT=2;\n'
                'void* heap_caps_malloc(std::size_t, unsigned);\n'
                'void heap_caps_free(void*);\n', encoding='utf-8')
            allocator_flags = ['-DARDUINO_ARCH_ESP32=1', '-DSPI_TEST_INTERNAL_HEAP=1', '-I' + tmp]
        command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                   *allocator_flags,
                   '-I' + str(ROOT / 'test/unit/cpp/spi_bus_mocks'),
                   '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
                   *map(str, sources), str(ROOT / 'src/core/SpiBusInitializer.cpp'), '-o', str(binary)]
        result = subprocess.run(command, capture_output=True, text=True,
                                encoding='utf-8', errors='replace', timeout=60)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        test.assertEqual(result.returncode, 0, result.stdout + result.stderr)


class SpiInitializerTests(unittest.TestCase):
    def test_real_driver_begin_failure_paths(self):
        def method(path, signature):
            source = (ROOT / path).read_text(encoding='utf-8')
            start = source.index(signature)
            brace = source.index('{', start)
            depth = 1
            end = brace + 1
            while depth:
                depth += (source[end] == '{') - (source[end] == '}')
                end += 1
            return source[start:end]

        template = (ROOT / 'test/unit/cpp/spi_driver_initialization_harness.cpp.in').read_text(encoding='utf-8')
        for token, path, signature in [
                ('RFID', 'src/peripherals/drivers/RFIDDriver.cpp',
                 'bool RFIDDriver::begin(uint8_t ssPin, uint8_t rstPin, uint8_t sckPin,'),
                ('SD', 'src/peripherals/SDCardManager.cpp',
                 'bool SDCardManager::ensureMountedLocked(String& errorMsg)'),
                ('SD_FAILURE', 'src/peripherals/SDCardManager.cpp',
                 'void SDCardManager::recordIoFailureLocked(const String& errorMsg)'),
                ('TFT', 'src/screen/SpiTftDisplayBackend.cpp',
                 'bool begin(const DisplayConfigParams& config, String& error) override')]:
            body = method(path, signature).replace(' override', '')
            template = template.replace('// @' + token + '_BEGIN@', body)
        generic = method('src/core/PeripheralManagerSPI.cpp',
                         'bool PeripheralManager::setupSpiHardware(const PeripheralConfig& config)')
        generic = generic.replace('PeripheralManager::setupSpiHardware(const PeripheralConfig& config)',
                                  'beginGeneric(const GenericConfig& config)')
        template = template.replace('// @GENERIC_BEGIN@', generic)
        self.assertNotIn('// @', template)
        with tempfile.TemporaryDirectory(prefix='fastbee-spi-drivers-') as tmp:
            source = Path(tmp) / 'drivers.cpp'
            source.write_text(template, encoding='utf-8')
            compile_and_run(self, [source])

    def test_real_initializer_failure_concurrency_and_recovery(self):
        compile_and_run(self, [ROOT / 'test/unit/cpp/spi_initializer_harness.cpp'])

    def test_real_leases_capacity_release_and_operation_lock(self):
        compile_and_run(self, [ROOT / 'test/unit/cpp/spi_lease_harness.cpp'])

    def test_lazy_storage_oom_and_retirement_lifecycle(self):
        compile_and_run(self, [ROOT / 'test/unit/cpp/spi_lazy_storage_harness.cpp'])

    def test_lazy_storage_uses_internal_byte_addressable_dram(self):
        compile_and_run(self, [ROOT / 'test/unit/cpp/spi_lazy_storage_harness.cpp'], internal_heap=True)

    def test_all_global_begin_paths_use_checked_initializer(self):
        files = ['src/core/PeripheralManagerSPI.cpp', 'src/peripherals/SDCardManager.cpp',
                 'src/peripherals/drivers/RFIDDriver.cpp', 'src/screen/SpiTftDisplayBackend.cpp']
        for name in files:
            with self.subTest(source=name):
                text = (ROOT / name).read_text(encoding='utf-8')
                entry = 'acquire'
                if name.endswith('PeripheralManagerSPI.cpp'):
                    entry = 'acquireNamed'
                self.assertIn('SpiBusInitializer::instance().' + entry + '(', text)
                self.assertNotIn('SPI.begin(', text)
                self.assertNotIn('_spi->begin(', text)
        manager = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        self.assertIn('return setupSpiHardware(config);', manager)
        self.assertIn('return teardownSpiHardware(config);', manager)


if __name__ == '__main__':
    unittest.main()
