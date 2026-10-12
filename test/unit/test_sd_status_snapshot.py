"""Real SD manager and shared JSON serializer: passive, bounded diagnostics."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SdStatusSnapshotTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        headers = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        if not compiler or not headers:
            raise unittest.SkipTest('C++17 and installed ArduinoJson required')
        cls.folder = tempfile.TemporaryDirectory(prefix='fastbee-sd-snapshot-')
        cls.addClassCleanup(cls.folder.cleanup)
        cls.binary = Path(cls.folder.name) / ('sd.exe' if os.name == 'nt' else 'sd')
        includes = ['test/unit/cpp/sd_bus_mocks', 'test/unit/cpp/spi_bus_mocks', 'test/mocks', 'include']
        sources = ['test/unit/cpp/sd_status_snapshot_harness.cpp', 'src/peripherals/SDCardManager.cpp',
                   'src/core/SpiBusInitializer.cpp', 'src/network/FixedJsonField.cpp']
        command = [compiler, '-std=c++17', '-pthread', '-Wall', '-Wextra', '-Werror',
                   '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                   *['-I' + str(ROOT / name) for name in includes], '-I' + str(headers[0].parent),
                   *[str(ROOT / name) for name in sources], '-o', str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_both_status_routes_use_shared_serializer(self):
        source = '\n'.join((ROOT / path).read_text(encoding='utf-8') for path in (
            'src/network/handlers/PeripheralRouteHandler.cpp',
            'src/network/handlers/PeripheralReadRoutes.cpp'))
        self.assertEqual(source.count('FastBee::writeSdCardStatusJson(data, status, id);'), 2)

    def test_every_sd_api_operation_carries_requested_owner(self):
        source = '\n'.join((ROOT / path).read_text(encoding='utf-8') for path in (
            'src/network/handlers/PeripheralRouteHandler.cpp',
            'src/network/handlers/PeripheralReadRoutes.cpp'))
        for call in ('list(path, entries, errorMsg, id, &truncated)', 'mount(errorMsg, id)',
                     'static_cast<size_t>(requestedMax), id)', 'content.length(), append, errorMsg, id)',
                     'removeFile(path, errorMsg, id)', 'unmount(id, &errorMsg, false)'):
            self.assertIn(call, source)


for _name in ('age_zero', 'age_multi_wrap', 'age_after_wrap', 'clock_rollback', 'clock_unavailable',
              'clock_unavailable_at_sample', 'age_refresh', 'cold', 'snapshot', 'storage_busy', 'bus_busy',
              'lost_bus', 'partial_write', 'unmount', 'invalid_capacity', 'json', 'owner'):
    setattr(SdStatusSnapshotTests, 'test_' + _name, lambda self, name=_name: self.run_case(name))

if __name__ == '__main__':
    unittest.main()
