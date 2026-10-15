"""Actual page migration acknowledgement/no-replay checks; raw backend coverage retained."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / 'test/unit/config_migration_web_harness.cjs'


class ConfigImportConfirmationTests(unittest.TestCase):
    def check(self, mode):
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required')
        result = subprocess.run([node, str(HARNESS), 'device', mode], cwd=ROOT,
            capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

    def test_routes_require_immediate_disk_commit(self):
        source = (ROOT/'src/network/handlers/SystemRouteHandler.cpp').read_text(encoding='utf-8')
        # D2 重构后业务下沉到 handle* 方法：覆盖 import/import-begin/import-chunk 三个处理器
        start = source.index('void SystemRouteHandler::handleConfigTransferImport(')
        end = source.index('#endif // FASTBEE_ENABLE_CONFIG_TRANSFER', start)
        block = source[start:end]
        self.assertEqual(block.count('configStorage.saveImportedJSONConfig(path, importedConfig)'), 2)
        self.assertNotIn('configStorage.saveJSONConfig(path, importedConfig)', block)
        self.assertIn('doc["saved"] = index == total - 1;', block)
        self.assertIn('doc["applied"] = false;', block)


for _case in ('success', 'unicode', 'lost_final_chunk', 'lost_middle_chunk', 'http400', 'http503',
              'chunk_name', 'chunk_size', 'chunk_old', 'chunk_false', 'chunk_numeric', 'chunk_session',
              'lost_begin', 'invalid_session', 'begin_phase', 'begin_numeric', 'occupied', 'local_oversize',
              'preview_session', 'preview_phase', 'preview_revision', 'preview_activation',
              'prepare_lost', 'prepare_session', 'prepare_revision', 'prepare_applied', 'prepare_restart'):
    setattr(ConfigImportConfirmationTests, 'test_'+_case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
