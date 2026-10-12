"""Peripheral page import: selected graph uses reviewed migration, never active per-file commit."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / 'test/unit/config_migration_web_harness.cjs'


class PeripheralConfigImportWebTests(unittest.TestCase):
    def check(self, kind, mode):
        result = subprocess.run(['node', str(HARNESS), kind, mode], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('success', 'warning', 'cancel', 'shape', 'too_large', 'preview_lost', 'unknown_risk',
              'preview_phase', 'preview_revision', 'begin_numeric', 'prepare_lost', 'preview_rejected',
              'prepare_revision', 'chunk_size', 'prepare_false', 'prepare_applied'):
    setattr(PeripheralConfigImportWebTests, 'test_' + _mode,
            lambda self, mode=_mode: self.check('peripheral', mode))


if __name__ == '__main__':
    unittest.main()
