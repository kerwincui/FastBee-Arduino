"""Permanent actual-SDK Basic regression and original failure controls."""
import unittest
from test.unit import test_web_finite_progress as finite


class WebBasicProgressTests(unittest.TestCase):
    def test_original_add_failure_empty_send_stalls_at_30s_and_wrap(self):
        result = finite.WebFiniteProgressTests.run_harness(self, basic=True, legacy_basic=True, expect_bounded=False)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertEqual(2, result.stdout.count("OLD_BASIC_STALL_REPRODUCED"))

    def test_actual_basic_deadlines_recovery_duplicate_ack_and_ownership(self):
        result = finite.WebFiniteProgressTests.run_harness(self, basic=True)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("BASIC_BOUNDED_PROGRESS_PASS", result.stdout)

    def test_original_basic_fails_the_bounded_regression(self):
        result = finite.WebFiniteProgressTests.run_harness(self, basic=True, legacy_basic=True)
        self.assertNotEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("f.basic->_state==RESPONSE_FAILED", result.stderr)


if __name__ == "__main__":
    unittest.main()
