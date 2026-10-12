"""Rule script page import stages unchanged templates for next-boot activation."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = ROOT / 'test/unit/config_migration_web_harness.cjs'


class RuleScriptImportWebTests(unittest.TestCase):
    def check(self, kind, mode):
        result = subprocess.run(['node', str(HARNESS), kind, mode], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _mode in ('success', 'lost_first_chunk', 'http400', 'chunk_name', 'chunk_size', 'preview_rejected',
              'prepare_false', 'prepare_applied', 'prepare_restart', 'prepare_revision',
              'prepare_numeric', 'prepare_lost', 'too_many', 'too_large', 'shape', 'preview'):
    setattr(RuleScriptImportWebTests, 'test_' + _mode,
            lambda self, mode=_mode: self.check('script', mode))


if __name__ == '__main__':
    unittest.main()
