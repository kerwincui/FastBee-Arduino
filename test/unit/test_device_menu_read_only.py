"""Keep legacy status POSTs distinct from movement/write commands."""
import pathlib
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


class DeviceMenuReadOnlyTests(unittest.TestCase):
    def test_panel_navigation_abort_contract_rejects_real_failures(self):
        result = subprocess.run(
            ['node', 'scripts/test-screen-panel-browser-device.mjs', '--contract-only'],
            cwd=ROOT, text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS: screen-panel browser contract', result.stdout)

    def test_status_post_allowlist_rejects_movement_and_ambiguous_payloads(self):
        source = r'''
import assert from 'node:assert/strict';
import {isReadOnlyBatch as batch, isReadOnlyMotorStatus as check} from './scripts/lib/read-only-device-requests.mjs';
const url = 'http://localhost/api/modbus/motor/control';
const form = 'application/x-www-form-urlencoded';
assert.ok(check('POST', url, 'action=readStatus&slaveAddress=8', form));
assert.ok(check('POST', url, '{"action":"readStatus","slaveAddress":8}', 'application/json'));
for (const action of ['forward', 'reverse', 'stop', 'setSpeed', 'setPulse'])
  assert.equal(check('POST', url, `action=${action}&slaveAddress=8`, form), false);
for (const body of ['action=readStatus&action=forward&slaveAddress=8',
 'action=readStatus&slaveAddress=8&value=1', 'action=readStatus&slaveAddress=0',
 'action=readStatus&slaveAddress=248', 'action=readStatus&slaveAddress=1e2',
 'action=readStatus&slaveAddress=8&slaveAddress=9', 'action=readStatus'])
  assert.equal(check('POST', url, body, form), false);
assert.equal(check('POST', url+'?action=forward', 'action=readStatus&slaveAddress=8', form), false);
assert.equal(check('POST', url, 'null', 'application/json'), false);
assert.equal(check('POST', url, '{', 'application/json'), false);
assert.equal(check('POST', url, 'action=readStatus&slaveAddress=8', 'text/plain'), false);
const batchUrl = 'http://localhost/api/batch';
const json = 'application/json; charset=utf-8';
assert.ok(batch('POST', batchUrl, '{"requests":[{"url":"/api/system/status"}]}', json));
assert.ok(batch('POST', batchUrl, '{"requests":[{"url":"/api/health","method":"GET","params":{"brief":1}}]}', json));
for (const body of [
  '{"requests":[]}',
  '{"requests":[{"url":"/api/modbus/motor/control","method":"POST","params":{"action":"forward","slaveAddress":8}}]}',
  '{"requests":[{"url":"/api/system/status","method":"DELETE"}]}',
  '{"requests":[{"url":"/api/system/status","extra":true}]}',
  '{"requests":[{"url":"http://example.test/api/status"}]}',
  '{"requests":[{"url":"/api/system/status"}],"mode":"write"}',
  '{"requests":[1]}', 'null', '{'
]) assert.equal(batch('POST', batchUrl, body, json), false);
assert.equal(batch('POST', batchUrl + '?mode=write', '{"requests":[{"url":"/api/health"}]}', json), false);
assert.equal(batch('POST', batchUrl, '{"requests":[{"url":"/api/health"}]}', 'text/plain'), false);
console.log('read-only menu request contract passed');
'''
        result = subprocess.run(['node', '--input-type=module', '--eval', source], cwd=ROOT,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_browser_scripts_block_unknown_writes_and_fail_on_diagnostics(self):
        panel = (ROOT / 'scripts/test-screen-panel-browser-device.mjs').read_text(encoding='utf-8')
        menus = (ROOT / 'scripts/test-web-device-menus.mjs').read_text(encoding='utf-8')
        for source in (panel, menus):
            self.assertIn("await page.route('**/api/**'", source)
            self.assertIn("isReadOnlyMotorStatus", source)
            self.assertIn("isReadOnlyBatch", source)
            self.assertIn("route.abort('blockedbyclient')", source)
            self.assertNotIn("['/api/auth/login', '/api/batch'].includes", source)
        self.assertIn("assert.deepEqual(report.httpErrors, []", panel)
        self.assertIn("assert.deepEqual(report.requestFailures, []", panel)
        self.assertIn("waitForMainShellStyles(page)", panel)
        self.assertIn("waitForMainShellStyles(page)", menus)
        self.assertIn("mainShellStyle: await waitForMainShellStyles(page)", menus)
        self.assertIn("!report.httpErrors.length", menus)
        self.assertIn("!report.requestFailures.length", menus)

    def test_panel_browser_requires_loaded_and_applied_main_styles(self):
        source = r'''
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { readFile } from 'node:fs/promises';
import { waitForMainShellStyles } from './scripts/lib/browser-main-style-check.mjs';
const startedAt = Date.now();
const phase = name => console.log(JSON.stringify({ phase: name, elapsedMs: Date.now() - startedAt }));
phase('module-ready');
const require = createRequire(import.meta.url);
const { chromium } = require('./test/browser/node_modules/playwright');
phase('launch:start');
const browser = await chromium.launch({ headless: true });
phase('launch:done');
phase('newPage:start');
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });
phase('newPage:done');
const shell = css => `<!doctype html><style id="fb-main-stylesheet" data-ready="1">${css}</style>
  <div id="app-container" class="app-container"><div class="app-wrapper">
    <aside id="sidebar" class="sidebar"></aside>
    <main class="main-content"><header class="header"></header><section class="content"></section></main>
  </div></div>`;
try {
  phase('empty-setContent:start');
  await page.setContent(shell(''));
  phase('empty-setContent:done');
  phase('empty-style-check:start');
  await assert.rejects(
    waitForMainShellStyles(page, { timeoutMs: 50, pollMs: 10 }),
    /Global main stylesheet is not applied:.*main CSS rule count too small/,
  );
  phase('empty-style-check:done');
  phase('main-css-read-and-setContent:start');
  await page.setContent(shell(await readFile('./web-src/css/main.css', 'utf8')));
  phase('main-css-read-and-setContent:done');
  phase('desktop-style-check:start');
  const evidence = await waitForMainShellStyles(page, { timeoutMs: 500, pollMs: 20 });
  assert.ok(evidence.ruleCount >= 5);
  assert.equal(evidence.computed.wrapperDisplay, 'flex');
  assert.ok(evidence.rects.sidebar.width >= 180);
  assert.ok(evidence.rects.main.left >= evidence.rects.sidebar.right - 1);
  phase('desktop-style-check:done');
  phase('mobile-viewport:start');
  await page.setViewportSize({ width: 390, height: 844 });
  phase('mobile-viewport:done');
  phase('mobile-style-check:start');
  const mobileEvidence = await waitForMainShellStyles(page, { timeoutMs: 500, pollMs: 20 });
  assert.equal(mobileEvidence.computed.wrapperDirection, 'column');
  phase('mobile-style-check:done');
  console.log('browser main style gate passed');
} finally {
  phase('close:start');
  await browser.close();
  phase('close:done');
}
'''
        try:
            result = subprocess.run(['node', '--input-type=module', '--eval', source], cwd=ROOT,
                                    capture_output=True, text=True, timeout=20)
        except subprocess.TimeoutExpired as error:
            def captured(value):
                return value.decode('utf-8', errors='replace') if isinstance(value, bytes) else (value or '')
            self.fail(
                f'Browser main style diagnostic timed out after {error.timeout}s; original timeout unchanged.\n'
                f'stdout:\n{captured(error.stdout)}\nstderr:\n{captured(error.stderr)}'
            )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
