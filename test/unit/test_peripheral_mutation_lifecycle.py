"""Actual PM lifecycle tails/rollback with simulated driver acknowledgements.

No device access. The production runtime-state struct is included by extraction;
driver setup/cleanup and unrelated pre-validation are the explicit host seams.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class PeripheralMutationLifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            headers = sorted((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not headers:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(headers[0].parent)
        if not (Path(json_root) / 'ArduinoJson.h').is_file():
            raise RuntimeError('FASTBEE_TEST_ARDUINOJSON_INCLUDE must contain ArduinoJson.h')
        source = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        header = (ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf-8')
        update = definition(source, 'bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg)')
        update = update[update.index('    const PeripheralConfig oldConfig ='):]
        methods = '\n'.join(definition(source, signature) for signature in (
            'bool PeripheralManager::restoreMutationSnapshot(',
            'bool PeripheralManager::enablePeripheral(',
            'bool PeripheralManager::deinitHardware(',
            'size_t PeripheralManager::suspendOptionalForMemory('))
        template = (ROOT / 'test/unit/cpp/peripheral_mutation_lifecycle_harness.cpp.in').read_text(encoding='utf-8')
        template = template.replace('// @state@', definition(header, 'struct PeripheralRuntimeState') + ';')
        template = template.replace('// @update@', update)
        template = template.replace('// @methods@', methods)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-mutation-lifecycle-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder / 'lifecycle.cpp'
        cpp.write_text(template, encoding='utf-8')
        cls.binary = folder / 'lifecycle.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/mocks'), '-I' + str(ROOT / 'include'),
            '-I' + json_root,
            str(cpp), '-o', str(cls.binary)], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('metadata', 'error_enable_rollback', 'error_enable_cleanup_failure',
              'state_change_rollback', 'suspended_update_success', 'suspended_update_restore',
              'suspended_new_cleanup_failure', 'suspended_old_cleanup_failure',
              'suspended_old_cleanup_success', 'ordinary_disabled_error',
              'suspension_failure', 'pending_enable', 'suspended_metadata', 'suspended_save_rollback'):
    setattr(PeripheralMutationLifecycleTests, 'test_' + _case, lambda self, case=_case: self.check(case))


if __name__ == '__main__':
    unittest.main()
