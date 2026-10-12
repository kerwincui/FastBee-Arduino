"""Reject reused Web evidence paths before copying, building or publishing."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WebConfigurationEvidenceTests(unittest.TestCase):
    def run_reused(self, path):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run(
            [node, str(ROOT / 'scripts/test-web-configuration.js'),
             '--build-only', '--output=' + str(path)],
            cwd=ROOT, capture_output=True, text=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Fresh Web configuration evidence directory required', result.stderr)

    def test_failed_evidence_is_never_overwritten(self):
        with tempfile.TemporaryDirectory(prefix='fastbee-web-evidence-') as temporary:
            out = Path(temporary) / 'failed-run'
            out.mkdir()
            original = {'build.log': b'original failure\n',
                        'summary.json': b'{"status":"FAIL"}\n',
                        'stage.json': b'{"stage":"original"}\n'}
            for name, content in original.items():
                (out / name).write_bytes(content)
            self.run_reused(out)
            self.assertEqual({p.name: p.read_bytes() for p in out.iterdir()}, original)

    def test_even_empty_existing_directory_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='fastbee-web-evidence-empty-') as temporary:
            self.run_reused(Path(temporary))
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_existing_file_is_not_replaced(self):
        with tempfile.TemporaryDirectory(prefix='fastbee-web-evidence-file-') as temporary:
            out = Path(temporary) / 'evidence'
            out.write_bytes(b'keep existing file')
            self.run_reused(out)
            self.assertEqual(out.read_bytes(), b'keep existing file')


if __name__ == '__main__':
    unittest.main()
