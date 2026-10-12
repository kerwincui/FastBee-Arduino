"""Exercise real JavaScript and real browser DOM; all checks remain offline."""
from pathlib import Path
import os
import subprocess
import unittest
from sd_recorder_schema_fixture import sd_recorder_schema_json

ROOT = Path(__file__).resolve().parents[2]


class SdRecorderWebTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.environment = {**os.environ, 'FASTBEE_TEST_SD_RECORDER_SCHEMA': sd_recorder_schema_json()}

    def test_form_schema_template_runtime_and_import(self):
        result = subprocess.run(['node', '--test', 'test/unit/test_sd_recorder_form.cjs'], cwd=ROOT,
            capture_output=True, text=True, encoding='utf-8', timeout=30, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_real_browser_editor(self):
        result = subprocess.run(['node', 'test/unit/test_sd_recorder_lazy_web.cjs'], cwd=ROOT,
            capture_output=True, text=True, encoding='utf-8', timeout=60, env=self.environment)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
