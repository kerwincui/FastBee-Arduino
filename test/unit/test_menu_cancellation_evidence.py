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

    def test_navigation_aborted_reads_exemption_keeps_real_failures(self):
        code = r"""
import assert from 'node:assert/strict';
import { isNavigatedAbortedRead as check } from './scripts/lib/menu-cancellation-evidence.mjs';
const ABORTED = 'net::ERR_ABORTED';
const get = route => ({ url: () => 'http://device.local' + route, method: () => 'GET',
  postData: () => null, headers: () => ({}) });
const batch = { url: () => 'http://device.local/api/batch', method: () => 'POST',
  postData: () => JSON.stringify({ requests: [{ url: '/api/system/info' }] }),
  headers: () => ({ 'content-type': 'application/json' }) };
const nav = { at: 1000, until: 4000, generation: 2, targetPage: 'device' };
// Idempotent reads interrupted by a menu navigation are exempt.
assert.equal(check(get('/api/system/info'), ABORTED, 1, 2, nav, 4600, 4500), true);
assert.equal(check(get('/api/system/info'), ABORTED, 2, 2, nav, 1500, 1400), true);
assert.equal(check(batch, ABORTED, 2, 2, nav, 1500, 1400), true);
assert.equal(check(get('/api/system/info'), ABORTED, 0, 0,
  { at: 1000, until: 4000, generation: 0, targetPage: 'app' }, 1500, 1400), true);
// Real failures still fail: other reasons, governor-timeout-aged aborts,
// aborts outside any navigation window, non-read-only or health requests.
assert.equal(check(get('/api/system/info'), 'net::ERR_TIMED_OUT', 1, 2, nav, 1500, 1400), false);
assert.equal(check(get('/api/system/info'), 'net::ERR_CONNECTION_RESET', 1, 2, nav, 1500, 1400), false);
assert.equal(check(get('/api/system/info'), ABORTED, 1, 2, nav, 16000, 1000), false);
assert.equal(check(get('/api/system/info'), ABORTED, 2, 2, null, 1500, 1400), false);
assert.equal(check(get('/api/system/info'), ABORTED, 2, 2, nav, 4001, 3900), false);
assert.equal(check(get('/api/system/info'), ABORTED, 2, 2, nav, 999, 900), false);
assert.equal(check(get('/api/system/info'), ABORTED, undefined, 2, nav, 1500, 1400), false);
assert.equal(check(get('/api/health'), ABORTED, 1, 2, nav, 1500, 1400), false);
const login = { url: () => 'http://device.local/api/auth/login', method: () => 'POST',
  postData: () => '{"username":"a"}', headers: () => ({ 'content-type': 'application/json' }) };
assert.equal(check(login, ABORTED, 1, 2, nav, 1500, 1400), false);
const writeBatch = { ...batch,
  postData: () => JSON.stringify({ requests: [{ url: '/api/system/info', method: 'POST' }] }) };
assert.equal(check(writeBatch, ABORTED, 1, 2, nav, 1500, 1400), false);
console.log('navigation aborted read contract passed');
"""
        result = subprocess.run(["node", "--input-type=module", "-e", code], cwd=ROOT,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
