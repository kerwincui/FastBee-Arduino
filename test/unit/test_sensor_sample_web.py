"""Exercise the actual monitor renderer and SSE handlers with an offline DOM seam."""
from pathlib import Path
import subprocess
import unittest


class SensorSampleWebTests(unittest.TestCase):
    def test_sample_age_unit_and_quality(self):
        result = subprocess.run(['node', '--test', 'test/unit/test_sensor_sample_web.cjs'],
                                cwd=Path(__file__).resolve().parents[2], capture_output=True,
                                text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
