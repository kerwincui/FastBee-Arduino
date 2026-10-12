"""Prepared real boot parser/import ownership and target-resource regressions."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_periph_exec_config_persistence import cpp_function

ROOT = Path(__file__).resolve().parents[2]


class PeripheralConfigImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-peripheral-import-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / ('import.exe' if os.name == 'nt' else 'import')
        source = (ROOT / 'src/core/PeripheralManagerPersistence.cpp').read_text(encoding='utf-8')
        helpers = source[source.index('namespace {'):source.index('} // namespace') + len('} // namespace')]
        methods = '\n'.join(cpp_function(source, 'bool PeripheralManager::' + name + '(') for name in
                            ('configurationImportPending', 'prepareConfigurationImport', 'commitConfigurationImport', 'saveConfiguration'))
        template = (ROOT / 'test/unit/cpp/peripheral_config_import_harness.cpp.in').read_text(encoding='utf-8')
        path = Path(cls.temp.name) / 'import.cpp'
        path.write_text(template.replace('HELPERS', helpers).replace('METHODS', methods), encoding='utf-8')
        includes = ['test/unit/fixtures/point_history', 'test/mocks',
                    '.pio/libdeps/esp32s3-F16R8/ArduinoJson/src', 'include', 'src/core']
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-DUNIT_TEST',
                                '-DCONFIG_IDF_TARGET_ESP32S3=1', '-DARDUINOJSON_ENABLE_STD_STRING=1',
                                '-DFASTBEE_ENABLE_IO_EXPANDER=1',
                                '-DFASTBEE_TEST_NEEDS_STRLCPY=1',
                                '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                                *['-I' + str(ROOT / part) for part in includes], str(path),
                                *[str(ROOT / ('src/core/' + part + '.cpp')) for part in
                                  ('Pca9685ConfigSupport', 'BoardProfile', 'PeripheralResourcePlanner', 'PeripheralNetworkResources')],
                                str(ROOT / 'src/network/FixedJsonField.cpp'),
                                '-o', str(cls.binary)], capture_output=True, text=True, timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('success', 'unknown', 'narrow', 'nul', 'duplicate', 'pin_conflict', 'capacity',
              'external_conflict', 'reference', 'missing_reference', 'busy', 'open', 'short', 'corrupt', 'rename', 'revision'):
    setattr(PeripheralConfigImportTests, 'test_' + _mode, lambda self, mode=_mode: self.check(mode))

if __name__ == '__main__':
    unittest.main()
