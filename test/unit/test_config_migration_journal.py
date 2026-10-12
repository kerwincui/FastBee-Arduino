"""Production boot transaction state machine with power loss at each durable step."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ConfigMigrationJournalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-migration-journal-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name)/'journal.exe'
        built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'include'), str(ROOT/'test/unit/cpp/config_migration_journal_harness.cpp'),
            '-o', str(cls.binary)], capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, mode):
        run = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
        self.assertEqual(run.returncode, 0, run.stdout+run.stderr)


for _mode in ('power-loss', 'recovery-power-loss', 'write-failure', 'failed-recovery',
              'candidate-corrupt', 'backup-corrupt', 'dependency-conflict', 'firmware-conflict',
              'markers', 'manifest', 'committed-user-edits'):
    setattr(ConfigMigrationJournalTests, 'test_'+_mode.replace('-', '_'),
            lambda self, mode=_mode: self.check(mode))
