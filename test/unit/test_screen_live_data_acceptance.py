"""Reject false-positive live acceptance when a bound card is empty or stale."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class ScreenLiveDataAcceptanceTests(unittest.TestCase):
    def test_live_acceptance_checks_real_bindings_and_quality(self):
        result = subprocess.run(
            ['node', 'scripts/test-screen-panel-live-data.mjs', '--contract-only'],
            cwd=ROOT, capture_output=True, text=True, timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('ten failure cases', result.stdout)
