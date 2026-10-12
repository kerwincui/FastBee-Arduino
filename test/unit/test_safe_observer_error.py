"""Synthetic exception tests execute real host checkers without a browser or device."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SafeObserverErrorTests(unittest.TestCase):
    def test_real_checkers_keep_cookie_errors_private_and_remain_failed(self):
        result = subprocess.run(['node', 'test/unit/test_safe_observer_error.mjs'], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
