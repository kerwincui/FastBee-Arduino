"""Screen/history page imports stage unchanged bytes and obey aggregate target admission."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / 'test/unit/config_migration_web_harness.cjs'


class ManagedConfigImportWebTests(unittest.TestCase):
    def check(self, kind, mode):
        result = subprocess.run(['node', str(HARNESS), kind, mode], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


COMMON = ('success', 'unknown_field', 'version', 'preview_lost', 'preview_rejected',
          'prepare_lost', 'prepare_revision', 'prepare_applied', 'prepare_numeric', 'cancel', 'preview')
for _kind, _extra in [('screen', ('old_layout', 'prepare_restart')),
                      ('history', ('duplicate', 'nul', 'too_many', 'prepare_session'))]:
    for _mode in COMMON + _extra:
        setattr(ManagedConfigImportWebTests, 'test_' + _kind + '_' + _mode,
                lambda self, kind=_kind, mode=_mode: self.check(kind, mode))


if __name__ == '__main__':
    unittest.main()
