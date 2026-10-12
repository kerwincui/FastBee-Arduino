"""Execute real PM mutation/rollback branches with injected driver outcomes.

Validation and individual drivers are outside this harness. It checks the
manager boundary: failed teardown must not erase or replace retained owners.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_screen_panel_contract import braced_block

ROOT = Path(__file__).resolve().parents[2]


class PeripheralCleanupRetentionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        source = (ROOT / 'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        body = lambda signature: braced_block(source, signature)
        add = body('bool PeripheralManager::addPeripheral(const PeripheralConfig& config, String& errorMsg)')
        update = body('bool PeripheralManager::updatePeripheral(const String& id, const PeripheralConfig& config, String& errorMsg)')
        enable = body('bool PeripheralManager::enablePeripheral(')
        restore = body('bool PeripheralManager::restoreMutationSnapshot(')
        replacements = {
            '// @add@': add[add.index('    // 添加外设'):],
            '// @update-stop@': 'if (oldConfig.enabled || (hadOldState && oldState.memoryCleanupPending)) {' + braced_block(update, 'if (oldConfig.enabled || (hadOldState && oldState.memoryCleanupPending))') + '}',
            '// @update-failure@': 'if (config.enabled && !initHardware(targetId)) {' + braced_block(update, 'if (config.enabled && !initHardware(targetId))') + '}',
            '// @enable-failure@': 'if (!initHardware(id)) {' + braced_block(enable, 'if (!initHardware(id))') + '}',
            '// @remove@': body('bool PeripheralManager::removePeripheral('),
            '// @rollback@': restore[restore.index('    auto removeCurrent ='):restore.index('    _memorySuspendedCount = snapshot.memorySuspendedCount;', restore.index('    auto removeCurrent ='))],
        }
        template = (ROOT / 'test/unit/cpp/peripheral_cleanup_retention_harness.cpp.in').read_text(encoding='utf-8')
        for marker, code in replacements.items():
            template = template.replace(marker, code)
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-cleanup-retention-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        cpp = folder / 'retention.cpp'
        cpp.write_text(template, encoding='utf-8')
        cls.binary = folder / 'retention.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I' + str(ROOT / 'test/mocks'), str(cpp), '-o', str(cls.binary)],
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=90)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    def check(self, mode):
        ran = subprocess.run([str(self.binary), mode], capture_output=True, text=True,
            encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_failed_add_cleanup_retains_configuration_and_owner(self): self.check('add')
    def test_failed_old_owner_shutdown_prevents_update(self): self.check('update-stop')
    def test_failed_new_owner_cleanup_prevents_old_configuration_restore(self): self.check('update-new')
    def test_failed_enable_cleanup_cannot_restore_disabled_configuration(self): self.check('enable')
    def test_failed_remove_preserves_owner_until_explicit_successful_retry(self): self.check('remove')
    def test_suspended_remove_changes_count_only_after_confirmed_cleanup(self): self.check('remove-suspended')
    def test_failed_persistence_rollback_retains_current_configuration(self): self.check('rollback')
    def test_driver_specific_and_generic_cleanup_errors_remain_visible(self): self.check('errors')


if __name__ == '__main__':
    unittest.main()
