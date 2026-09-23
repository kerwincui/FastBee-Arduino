import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class MenuCancellationEvidenceTests(unittest.TestCase):
    def test_only_explicit_stale_health_cancellations_are_excluded(self):
        code = r"""
import assert from 'node:assert/strict';
import { isIntentionalHealthCancellation as classify } from './scripts/lib/menu-cancellation-evidence.mjs';
const failed = { route: '/api/health', method: 'GET', reason: 'net::ERR_ABORTED', at: 150 };
const observed = { route: '/api/health', pageAborted: true, requestTimeout: false,
  startGeneration: 3, endGeneration: 4, startedAt: 100, finishedAt: 151 };
assert.equal(classify(failed, [observed]), true);
assert.equal(classify(failed, []), false);
for (const change of [{ method: 'POST' }, { route: '/api/features/status' },
  { reason: 'net::ERR_CONNECTION_RESET' }, { at: 99 }, { at: 2000 }])
  assert.equal(classify({ ...failed, ...change }, [observed]), false);
for (const change of [{ pageAborted: false }, { requestTimeout: true },
  { endGeneration: 3 }, { startGeneration: undefined }, { route: '/api/system/status' }])
  assert.equal(classify(failed, [{ ...observed, ...change }]), false);
"""
        result = subprocess.run(["node", "--input-type=module", "-e", code], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
