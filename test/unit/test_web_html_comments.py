"""Publish-only static comment removal preserves the browser DOM and form data."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class WebHtmlCommentsTests(unittest.TestCase):
    def test_production_writer_browser_equivalence(self):
        result = subprocess.run(['node', 'test/unit/test_web_html_comments.cjs', '--env=esp32s3-F8R0'],
            cwd=ROOT, capture_output=True, text=True, encoding='utf-8', timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
