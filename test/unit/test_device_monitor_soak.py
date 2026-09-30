"""Run offline JavaScript soak evidence and runner regressions in the host suite."""
from pathlib import Path
import subprocess
import unittest

class BrowserSoakRegressionTests(unittest.TestCase):
    def test_offline_browser_soak_regressions(self):
        root = Path(__file__).resolve().parents[2]
        result = subprocess.run(['node', '--test', 'test/unit/test_device_monitor_soak.cjs'],
                                cwd=root, capture_output=True, text=True, timeout=40)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
