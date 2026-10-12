"""Actual NC01 form serializer and safety choices, without any API/device call."""
from pathlib import Path
import os
import subprocess
import unittest
from thermostat_schema_fixture import thermostat_schema_json


class ThermostatFormTests(unittest.TestCase):
    def test_form_behaviors(self):
        result = subprocess.run(['node', '--test', 'test/unit/test_thermostat_form.cjs'],
            cwd=Path(__file__).resolve().parents[2], capture_output=True,
            text=True, encoding='utf-8', timeout=30,
            env={**os.environ, 'FASTBEE_TEST_THERMOSTAT_SCHEMA': thermostat_schema_json()})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
