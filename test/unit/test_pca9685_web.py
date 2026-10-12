"""Prepared real Web modal request/state tests, without device access."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Pca9685WebTests(unittest.TestCase):
    def check(self, case):
        node = shutil.which('node')
        if not node:
            raise RuntimeError('Node required for production Web module tests')
        ran = subprocess.run([node, str(ROOT / 'test/helpers/pca9685-web-harness.js'), case],
                             capture_output=True, text=True, timeout=15)
        self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)


for _case in ('fresh_review', 'unknown_start', 'late_response', 'readonly_refresh',
              'malformed_status', 'wrong_identity', 'literal_input', 'developer_required',
              'control_conflict', 'transport_control_rejected', 'foreign_control_rejected',
              'foreign_nested_control_rejected'):
    setattr(Pca9685WebTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
