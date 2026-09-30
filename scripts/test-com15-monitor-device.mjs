import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { isReadOnlyBatch } from './lib/read-only-device-requests.mjs';

const require = createRequire(import.meta.url);
const { chromium, expect: baseExpect } = require('../test/browser/node_modules/@playwright/test');
const expect = baseExpect.configure({ timeout: 45000 });
const base = process.env.FASTBEE_BASE_URL;
const password = process.env.FASTBEE_PASSWORD;
const expectedMac = process.env.FASTBEE_EXPECTED_MAC;
assert(base && password && expectedMac && process.argv[2],
  'Set FASTBEE_BASE_URL, FASTBEE_PASSWORD, FASTBEE_EXPECTED_MAC and an output directory');
const output = path.resolve(process.argv[2]);
fs.mkdirSync(output, { recursive: false });
const report = { at: new Date().toISOString(), displayPassed: false, passed: false,
  measurementsAvailable: false, relayStatesAvailable: false,
  checks: [], httpErrors: [], blocked: [], pageErrors: [], controlsResponses: [] };
const browser = await chromium.launch({ headless: true });
const context = await browser.newContext({ viewport: { width: 1440, height: 1000 }, serviceWorkers: 'block' });
const page = await context.newPage();
page.setDefaultTimeout(45000);
await context.route('**/api/**', async route => {
  const request = route.request(), pathname = new URL(request.url()).pathname;
  const batch = isReadOnlyBatch(request.method(), request.url(), request.postData() || '',
    request.headers()['content-type'] || '');
  const busWrite = /^\/api\/modbus\/(?:coil\/(?:control|batch|delay)|register\/(?:write|batch-write)|motor\/control|device\/(?:address|baudrate))/.test(pathname);
  if ((!['GET', 'HEAD'].includes(request.method()) && pathname !== '/api/auth/login' && !batch) || busWrite) {
    report.blocked.push({ path: pathname, method: request.method() });
    await route.abort('blockedbyclient');
    return;
  }
  await route.continue();
});
page.on('pageerror', error => report.pageErrors.push(error.message));
page.on('response', response => {
  if (response.status() >= 400) report.httpErrors.push({ path: new URL(response.url()).pathname, status: response.status() });
  if (new URL(response.url()).pathname === '/api/periph-exec/controls') {
    response.json().then(body => report.controlsResponses.push({
      status: response.status(), success: body.success, degraded: body.degraded === true,
      sensorRules: body.data?.sensor?.length ?? null,
    })).catch(() => {});
  }
});

async function measure(name) {
  await expect(page.locator('.dc-monitor-card')).toHaveCount(9);
  await page.waitForFunction(() => window.AppState._dcDeviceOnline?.[0] !== undefined,
    null, { timeout: 20000 });
  const snapshot = await page.evaluate(() => {
    const app = window.AppState, tasks = app._modbusStatus?.tasks || [];
    const valid = sample => sample && sample.valid !== false &&
      (!sample.quality || ['valid', 'good', 'ok'].includes(String(sample.quality).toLowerCase())) &&
      ['number', 'string'].includes(typeof sample.value) && String(sample.value).trim() !== '' &&
      Number.isFinite(Number(sample.value));
    const expected = [];
    for (const task of tasks.filter(item => item.enabled)) for (const mapping of task.mappings || []) {
      if (!mapping.sensorId) continue;
      let value = '--';
      const cache = task.cachedData, offset = mapping.regOffset ?? 0, type = mapping.dataType ?? 0;
      const count = type >= 2 && type <= 4 ? 2 : 1, words = cache?.values;
      if (task.status === 'ok' && cache?.valid !== false && !task.lastError && !cache?.lastError &&
          Number.isInteger(offset) && offset >= 0 && Number.isInteger(type) && type >= 0 && type <= 4 &&
          Array.isArray(words) && offset + count <= words.length &&
          words.slice(offset, offset + count).every(word => Number.isInteger(word) && word >= 0 && word <= 65535)) {
        let raw = words[offset];
        if (type === 1 && raw > 32767) raw -= 65536;
        if (count === 2) {
          raw = raw * 65536 + words[offset + 1];
          if (type === 3 && raw > 2147483647) raw -= 4294967296;
          if (type === 4) {
            const view = new DataView(new ArrayBuffer(4));
            view.setUint32(0, raw); raw = view.getFloat32(0);
          }
        }
        const scale = mapping.scaleFactor === undefined ? 1 : mapping.scaleFactor;
        if (typeof scale === 'number' && Number.isFinite(raw * scale))
          value = (raw * scale).toFixed(mapping.decimalPlaces || 0);
      }
      const live = task._dcMappedValues?.[mapping.sensorId];
      if (live) value = valid(live) ? String(live.value) : '--';
      expected.push({ id: mapping.sensorId, value });
    }
    const cards = Array.from(document.querySelectorAll('.dc-monitor-card')).map(card => ({
      id: card.dataset.id, value: card.querySelector('.dc-monitor-value')?.textContent,
      unit: card.querySelector('.dc-monitor-unit')?.textContent || '',
    }));
    const coil = Array.from(document.querySelectorAll('.dc-coil-card')).map(card => ({
      channel: card.dataset.ch, text: card.querySelector('.dc-coil-st')?.textContent,
      unknown: card.classList.contains('dc-coil-unknown'),
    }));
    return { cards, expected, coil };
  });
  snapshot.mismatches = snapshot.expected.filter(item =>
    snapshot.cards.find(card => card.id === item.id)?.value !== item.value);
  report.checks.push({ name, ...snapshot });
  await page.screenshot({ path: path.join(output, name + '.png'), fullPage: true,
    mask: [page.locator('input[type="password"]')] });
  expect(snapshot.expected).toHaveLength(7);
  expect(snapshot.mismatches).toEqual([]);
  expect(snapshot.cards.filter(card => card.id.startsWith('ai_soil_'))).toHaveLength(2);
  expect(snapshot.coil).toHaveLength(4);
}

try {
  await page.goto(base, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__fastbeeBootReady === true, null, { timeout: 45000 });
  await page.locator('#username').fill(process.env.FASTBEE_USERNAME || 'admin');
  await page.locator('#password').fill(password);
  await page.locator('#login-button').click();
  await expect(page.locator('#app-container')).toBeVisible();
  const info = await page.evaluate(async () => (await fetch('/api/system/info')).json());
  const normalize = text => String(text || '').replace(/[^0-9a-f]/gi, '').toLowerCase();
  assert.equal(normalize(expectedMac).length, 12);
  assert.equal(normalize(info.data?.network?.macAddress), normalize(expectedMac), 'Device MAC mismatch');
  await page.locator('.menu-item[data-page="device-control"]').click();
  await measure('initial');
  await page.locator('#dc-refresh-btn').click();
  await expect(page.locator('#dc-refresh-btn')).toBeEnabled();
  await measure('refresh');
  await page.locator('.menu-item[data-page="peripheral"]').click();
  await expect(page.locator('#peripheral-table-body')).toBeVisible();
  await page.locator('.menu-item[data-page="device-control"]').click();
  await measure('navigation');
  report.displayPassed = true;
  report.measurementsAvailable = report.checks.every(check => check.cards.every(item =>
    typeof item.value === 'string' && item.value.trim() !== '' && Number.isFinite(Number(item.value))));
  report.relayStatesAvailable = report.checks.every(check => check.coil.every(item => !item.unknown));
  report.passed = report.displayPassed && report.measurementsAvailable && report.relayStatesAvailable &&
    !report.httpErrors.length && !report.blocked.length && !report.pageErrors.length;
} catch (error) {
  report.error = error.message.split(password).join('[redacted]');
  await page.screenshot({ path: path.join(output, 'failure.png'), fullPage: true,
    mask: [page.locator('input[type="password"]')] }).catch(() => {});
} finally {
  await browser.close();
  fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify({ output, displayPassed: report.displayPassed, passed: report.passed,
    measurementsAvailable: report.measurementsAvailable, httpErrors: report.httpErrors,
    relayStatesAvailable: report.relayStatesAvailable,
    pageErrors: report.pageErrors, blocked: report.blocked, error: report.error }, null, 2));
  process.exitCode = report.passed ? 0 : 1;
}
