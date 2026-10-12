"""Actual production minification/writer paths and encoded lazy-module behavior."""
from pathlib import Path
import os
import subprocess
import unittest
from sd_recorder_schema_fixture import sd_recorder_schema_json

ROOT = Path(__file__).resolve().parents[2]


class WebPrivatePropertiesTests(unittest.TestCase):
    def test_published_runtime_contract(self):
        environment = {**os.environ, 'FASTBEE_TEST_SD_RECORDER_SCHEMA': sd_recorder_schema_json()}
        result = subprocess.run(['node', 'test/unit/test_web_private_properties.cjs', '--env=esp32s3-F8R0'],
            cwd=ROOT, env=environment, capture_output=True, text=True, encoding='utf-8', timeout=180)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
