"""Exercise actual rule save/preflight JS with offline transport and DOM seams."""
from pathlib import Path
import subprocess
import unittest


class RuleActionContractWebTests(unittest.TestCase):
    def test_preflight_and_save_behavior(self):
        result = subprocess.run(['node', '--test', 'test/unit/test_rule_action_contract_web.cjs'],
            cwd=Path(__file__).resolve().parents[2], capture_output=True,
            text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
