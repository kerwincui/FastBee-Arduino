"""Real offline ADS form and channel consumers with production metadata."""
import json
import os
from pathlib import Path
import subprocess
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[2]


class Ads1115FormTests(unittest.TestCase):
    def test_actual_form_and_consumers(self):
        evidence_root = Path(os.environ.get('FASTBEE_TEST_ARTIFACT_ROOT', ROOT / 'artifacts'))
        evidence_root.mkdir(parents=True, exist_ok=True)
        output = evidence_root / ('ads1115-form-host-' + uuid.uuid4().hex)
        result = subprocess.run(['node', 'scripts/test-ads1115-form.cjs', str(output)],
            cwd=ROOT, capture_output=True, text=True, encoding='utf-8', timeout=120)
        print(result.stdout)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = json.loads((output / 'report.json').read_text(encoding='utf-8'))
        self.assertEqual(report['status'], 'PASS')
        self.assertTrue(report['inputsStable'])
        self.assertEqual(len(report['cases']), 50)
        self.assertTrue(all(case['status'] == 'PASS' for case in report['cases']))
