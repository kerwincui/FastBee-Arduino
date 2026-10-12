"""C++17 multi-translation-unit linkage for actual H1/NC metadata tables."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SharedMetadataStorageTests(unittest.TestCase):
    def test_actual_metadata_has_shared_storage_and_constexpr_access(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        with tempfile.TemporaryDirectory(prefix='fastbee-shared-metadata-') as folder:
            binary = Path(folder) / ('metadata.exe' if os.name == 'nt' else 'metadata')
            built = subprocess.run([
                compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'include'),
                str(ROOT / 'test/unit/cpp/shared_metadata_main.cpp'),
                str(ROOT / 'test/unit/cpp/shared_metadata_peer.cpp'),
                '-o', str(binary),
            ], capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            ran = subprocess.run([str(binary)], capture_output=True, text=True,
                                 encoding='utf-8', errors='replace', timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


if __name__ == '__main__':
    unittest.main()
