import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class RequestGovernorPressureTests(unittest.TestCase):
    def test_batches_are_deduplicated_bounded_and_navigation_safe(self):
        result = subprocess.run(["node", str(ROOT / "test/unit/node_request_governor_batch.cjs")],
                                cwd=ROOT, capture_output=True, text=True, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_constrained_static_and_api_requests_share_one_transport_budget(self):
        result = subprocess.run(["node", str(ROOT / "test/unit/node_shared_web_transport.cjs")],
                                cwd=ROOT, capture_output=True, text=True, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)

    def test_catalog_cache_invalidation_and_read_cooldown(self):
        result = subprocess.run(["node", str(ROOT / "test/unit/node_request_governor_pressure.cjs")],
                                cwd=ROOT, capture_output=True, text=True, timeout=15)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
