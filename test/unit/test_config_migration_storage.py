"""Actual LittleFS adapter using byte-backed FS and a noncryptographic SDK hash seam."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ConfigMigrationStorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-migration-storage-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name)/'storage.exe'
        command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
            '-I'+str(ROOT/'test/unit/cpp/migration_mocks'), '-I'+str(ROOT/'include'),
            str(ROOT/'test/unit/cpp/config_migration_storage_harness.cpp'),
            str(ROOT/'src/systems/ConfigMigrationStorage.cpp'), '-o', str(cls.binary)]
        built = subprocess.run(command, capture_output=True, text=True, timeout=60)
        if built.returncode:
            raise AssertionError(built.stdout+built.stderr)

    def check(self, mode):
        result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


for _mode in ('staging', 'original-range', 'original-backup', 'review', 'commit', 'conflict', 'corrupt', 'capacity-hash-short-write',
              'install-failure', 'power-loss', 'discard-power-loss', 'discard-guards', 'orphan-preservation',
              'orphan-guards', 'orphan-rename-failure', 'orphan-power-loss', 'protected-paths'):
    setattr(ConfigMigrationStorageTests, 'test_'+_mode.replace('-', '_'),
            lambda self, mode=_mode: self.check(mode))
