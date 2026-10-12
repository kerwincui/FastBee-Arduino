"""Actual configuration page migration flow; no network or device operations."""
from pathlib import Path
import subprocess
import unittest


class ConfigMigrationWebTests(unittest.TestCase):
    def test_migration_workflow(self):
        result = subprocess.run(['node', '--test', 'test/unit/test_config_migration_web.cjs'],
            cwd=Path(__file__).resolve().parents[2], capture_output=True,
            text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
