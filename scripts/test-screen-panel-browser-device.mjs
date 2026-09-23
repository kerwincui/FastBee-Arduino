import { createRequire } from 'node:module';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
import { isReadOnlyBatch, isReadOnlyMotorStatus } from './lib/read-only-device-requests.mjs';
import { waitForMainShellStyles } from './lib/browser-main-style-check.mjs';

const require = createRequire(import.meta.url);
const { chromium } = require('../test/browser/node_modules/playwright');
const argument = (key, fallback) => process.argv.find((value) => value.startsWith(`--${key}=`))?.slice(key.length + 3) || fallback;
const baseUrl = argument('url', 'http://192.168.5.26').replace(/\/$/, '');
const output = path.resolve(argument('output', 'artifacts/screen-panel-browser-device'));
await mkdir(output, { recursive: true });
const report = { baseUrl, at: new Date().toISOString(), passed: false, panelPreviewTested: false, mutations: [], readOnlyPosts: [], pageErrors: [], httpErrors: [], loginRequests: 0, requestFailures: [] };
const browser = await chromium.launch({ headless: true });
const exerciseSwitch = process.argv.includes('--exercise-switch');
const restoreOnly = process.argv.includes('--restore-only');
const allowFeatureMutation = exerciseSwitch || restoreOnly;
let page;
let restoreSwitch = false;
let closing = false;
async function setPanelEnabled(enabled) {
  const result = await page.evaluate(async (value) => {
    const response = await fetch('/api/features/config', {
      method: 'POST', headers: { 'Content-Type': 'application/json', ...window.AppState.getAuthHeader() },
      signal: AbortSignal.timeout(15_000),
      body: JSON.stringify({ features: { screenPanel: { enabled: value } } }),
    });
    return { status: response.status, body: await response.json() };
  }, enabled);
  assert.equal(result.status, 200);
  assert.equal(result.body.success, true);
  console.log(`Feature switch ${enabled ? 'enabled' : 'disabled'}`);
}
try {
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, serviceWorkers: 'block' });
  page = await context.newPage();
  const isStatusRead = request => isReadOnlyMotorStatus(request.method(), request.url(), request.postData(), request.headers()['content-type']);
  const isBatchRead = request => isReadOnlyBatch(request.method(), request.url(), request.postData(), request.headers()['content-type']);
  await page.route('**/api/**', async route => {
    const request = route.request();
    const pathname = new URL(request.url()).pathname;
    const login = request.method() === 'POST' && pathname === '/api/auth/login';
    const featureExercise = allowFeatureMutation && request.method() === 'POST' && pathname === '/api/features/config';
    if (['GET', 'HEAD', 'OPTIONS'].includes(request.method()) || login || featureExercise ||
        isBatchRead(request) || isStatusRead(request)) await route.continue();
    else await route.abort('blockedbyclient');
  });
  page.setDefaultTimeout(30_000);
  page.on('pageerror', (error) => report.pageErrors.push(error.message));
  page.on('requestfailed', request => {
    if (!closing) report.requestFailures.push({
      route: new URL(request.url()).pathname, method: request.method(), error: request.failure()?.errorText,
    });
  });
  page.on('response', response => {
    if (response.status() >= 400) report.httpErrors.push({
      route: new URL(response.url()).pathname, method: response.request().method(), status: response.status(),
    });
  });
  page.on('request', (request) => {
    const route = new URL(request.url()).pathname;
    if (request.method() === 'POST' && route === '/api/auth/login') ++report.loginRequests;
    if (isStatusRead(request)) report.readOnlyPosts.push({ method: request.method(), route, action: 'readStatus' });
    else if (isBatchRead(request)) report.readOnlyPosts.push({ method: request.method(), route, action: 'batchGet' });
    if (!['GET', 'HEAD', 'OPTIONS'].includes(request.method())) {
      const login = request.method() === 'POST' && route === '/api/auth/login';
      const featureExercise = allowFeatureMutation && request.method() === 'POST' && route === '/api/features/config';
      if (!login && !featureExercise && !isBatchRead(request) && !isStatusRead(request))
        report.mutations.push({ method: request.method(), route });
      else if (featureExercise) report.mutations.push({ method: request.method(), route });
    }
  });
  await page.goto(baseUrl, { waitUntil: 'domcontentloaded', timeout: 45_000 });
  await page.waitForFunction(() => window.__fastbeeBootReady === true, { timeout: 45_000 });
  await page.locator('#username').fill(process.env.FASTBEE_USERNAME || 'admin');
  await page.locator('#password').fill(process.env.FASTBEE_PASSWORD || 'admin123');
  const loginResponsePromise = page.waitForResponse((response) =>
    new URL(response.url()).pathname === '/api/auth/login' && response.request().method() === 'POST',
  { timeout: 30_000 });
  await page.locator('#login-button').click();
  const loginResponse = await loginResponsePromise;
  report.loginStatus = loginResponse.status();
  assert.equal(report.loginStatus, 200, 'Browser login status');
  await page.locator('#app-container').waitFor({ state: 'visible', timeout: 45_000 });
  report.mainShellStyle = await waitForMainShellStyles(page);
  console.log('Browser authenticated');
  if (restoreOnly) {
    await setPanelEnabled(false);
    report.originalFeatureRestored = true;
    report.passed = true;
  } else {
  report.memory = await page.evaluate(async () => {
    const response = await fetch('/api/system/status', { cache: 'no-store', signal: AbortSignal.timeout(15_000) });
    if (!response.ok) throw new Error(`System status HTTP ${response.status}`);
    const body = await response.json();
    return body.data?.memory || {};
  });
  await page.waitForFunction(() => window.AppState?._featureStatusReady === true);
  const feature = await page.evaluate(async () => {
    const response = await fetch('/api/features/status', { cache: 'no-store', signal: AbortSignal.timeout(15_000) });
    if (!response.ok) throw new Error(`Feature status HTTP ${response.status}`);
    const body = await response.json();
    const panel = body.data.features.find((item) => item.key === 'screenPanel');
    return { compiled: panel?.compiled, userEnabled: panel?.userEnabled, active: panel?.active };
  });
  report.feature = { ...feature };
  assert.equal(feature.compiled, true, 'S3 panel must be compiled');
  if (!feature.userEnabled && exerciseSwitch) {
    const displayRef = await page.evaluate(async () => {
      const response = await fetch('/api/screen-panel/config', { cache: 'no-store', signal: AbortSignal.timeout(15_000) });
      const body = await response.json();
      return body.data.config.displayRef;
    });
    assert.ok(!displayRef, 'Temporary enable requires no configured display hardware');
    restoreSwitch = true;
    await setPanelEnabled(true);
    await page.reload({ waitUntil: 'domcontentloaded', timeout: 30_000 });
    await page.locator('#app-container').waitFor({ state: 'visible', timeout: 30_000 });
    report.mainShellStyleAfterReload = await waitForMainShellStyles(page);
    await page.waitForFunction(() => window.AppState?._featureStatusReady === true);
    feature.userEnabled = true;
    report.temporarilyEnabled = true;
  }
  if (feature.userEnabled) {
    await page.locator('.menu-item[data-page="screen-panel"]').click();
    await page.locator('#screen-panel-page.active').waitFor({ state: 'visible' });
    await page.waitForFunction(() => window.AppState?._spModel?.loaded === true);
    await page.locator('#sp-tab-config').click();
    await page.locator('#sp-config').waitFor({ state: 'visible' });
    assert.equal(await page.locator('#sp-editor-canvas').isVisible(), false,
      'Layout editor belongs to Preview, not hardware configuration');
    const savedLayout = await page.evaluate(async () => {
      const response = await fetch('/api/screen-panel/config', {
        headers: window.AppState.getAuthHeader(), signal: AbortSignal.timeout(15000),
      });
      if (!response.ok) throw new Error(`Panel config HTTP ${response.status}`);
      return (await response.json()).data;
    });
    await page.locator('#sp-tab-preview').click();
    await page.locator('#sp-editor-canvas').waitFor({ state: 'visible' });
    const pageTabs = page.locator('#sp-page-tabs .sp-page-tab');
    const savedPageCount = savedLayout.config.pages.length;
    assert.equal(await pageTabs.count(), savedPageCount);
    report.savedPageCount = savedPageCount;
    if (process.argv.includes('--verify-weather')) {
      const weatherPage = savedLayout.config.pages.findIndex(item =>
        item.cards.some(card => card.kind === 'weather' && card.sourceRef));
      assert.ok(weatherPage >= 0, 'A bound weather card is required');
      await pageTabs.nth(weatherPage).click();
      const weatherCard = page.locator('#sp-editor-canvas [data-kind="weather"]').first();
      await weatherCard.waitFor({ state: 'visible' });
      const icon = weatherCard.locator('.sp-weather-icon').first();
      assert.ok(await icon.isVisible(), 'Weather icon must be visible');
      assert.notEqual(await icon.getAttribute('data-icon-id'), 'unknown', 'Weather needs a real condition');
      assert.match(await weatherCard.locator('.sp-card-value').first().innerText(), /\d/,
        'Weather temperature must be rendered');
      await page.screenshot({ path: path.join(output, 'weather.png'), fullPage: true });
      report.weatherVisualizationTested = true;
    }
    if (process.argv.includes('--verify-system')) {
      const systemPage = savedLayout.config.pages.findIndex(item =>
        item.cards.some(card => card.kind === 'system' && card.systemActionId === 'brightness'));
      assert.ok(systemPage >= 0, 'System controls page is required');
      const cards = savedLayout.config.pages[systemPage].cards;
      const actions = cards.filter(card => card.kind === 'system');
      for (const action of ['brightness', 'sleep', 'buzzer', 'restart']) {
        assert.ok(savedLayout.capabilities.systemActions.includes(action), `Runtime capability ${action}`);
        assert.ok(actions.some(card => card.systemActionId === action), `Missing ${action} card`);
      }
      const levels = actions.filter(card => card.systemActionId === 'brightness').map(card => card.actionValue);
      assert.ok(levels.length >= 2 && levels.every(value => value >= 1 && value <= 100));
      const restart = actions.find(card => card.systemActionId === 'restart');
      assert.equal(restart.requireConfirmation, true, 'Restart must require confirmation');
      assert.equal(savedLayout.config.settings.allowSystemRestart, true);
      await pageTabs.nth(systemPage).click();
      const brightness = page.locator('#sp-editor-canvas [data-kind="brightness"]').first();
      await brightness.waitFor({ state: 'visible' });
      await brightness.click(); // Web preview selects the card; it must never invoke hardware.
      await page.screenshot({ path: path.join(output, 'system.png'), fullPage: true });
      report.systemControlsPreviewTested = true;
      report.brightnessPresets = levels;
    }
    if (savedPageCount < savedLayout.capabilities.maxPages) {
      await page.locator('#sp-page-preset').selectOption('blank');
      page.once('dialog', dialog => dialog.accept('临时验收草稿'));
      await page.locator('#sp-page-add').click();
      assert.equal(await pageTabs.count(), savedPageCount + 1);
      report.unsavedPageAppendTested = true;
    }
    // Return to the real first page for screenshots. Never save the test draft.
    await pageTabs.first().click();
    await page.locator('#sp-tab-preview').click();
    const persistedRevision = await page.evaluate(async () => {
      const response = await fetch('/api/screen-panel/config', {
        headers: window.AppState.getAuthHeader(), signal: AbortSignal.timeout(15000),
      });
      if (!response.ok) throw new Error(`Panel config HTTP ${response.status}`);
      return (await response.json()).data.revision;
    });
    assert.equal(persistedRevision, savedLayout.revision, 'Draft test must not persist changes');
    report.panelPreviewTested = true;
  } else {
    assert.equal(await page.locator('.menu-item[data-page="screen-panel"]').isVisible(), false);
    await page.locator('.menu-item[data-page="features"]').click();
    await page.locator('#features-page.active').waitFor({ state: 'visible' });
    report.limitation = 'Panel disabled in existing user configuration; only capability visibility and feature page verified.';
  }
  for (const [name, width, height] of [['desktop', 1440, 900], ['mobile', 390, 844]]) {
    await page.setViewportSize({ width, height });
    if (report.panelPreviewTested) {
      await page.locator('#sp-editor-canvas').waitFor({ state: 'visible' });
      if (name === 'mobile') {
        const canvas = await page.locator('.sp-canvas-column').boundingBox();
        const toolbox = await page.locator('.sp-toolbox').boundingBox();
        assert.ok(canvas && toolbox && canvas.y < toolbox.y, 'Mobile preview must precede the component library');
      }
    }
    const viewport = await page.evaluate(() => ({ width: innerWidth, document: document.documentElement.scrollWidth }));
    assert.ok(viewport.document <= viewport.width + 1, `${name} horizontal overflow`);
    await page.screenshot({ path: path.join(output, `${name}.png`), fullPage: true });
  }
  assert.ok(report.mutations.every((request) => allowFeatureMutation && request.route === '/api/features/config'),
    'Browser acceptance must not save panel/peripherals or execute controls');
  assert.deepEqual(report.pageErrors, [], 'Browser must not throw uncaught errors');
  assert.deepEqual(report.httpErrors, [], 'Browser requests must not return HTTP errors');
  assert.deepEqual(report.requestFailures, [], 'Browser requests must not fail or be aborted');
  report.passed = true;
  }
} catch (error) {
  report.error = error.message;
  process.exitCode = 1;
  if (page && !page.isClosed()) {
    report.failureState = await page.evaluate(() => ({
      bootReady: window.__fastbeeBootReady === true,
      loginHandler: typeof window.AppState?.handleLogin,
      loginDisabled: document.querySelector('#login-button')?.disabled,
    })).catch(() => null);
    await page.screenshot({ path: path.join(output, 'failure.png'), fullPage: true }).catch(() => {});
  }
} finally {
  if (restoreSwitch && page && !page.isClosed()) {
    try {
      await setPanelEnabled(false);
      report.originalFeatureRestored = true;
    } catch (error) {
      report.passed = false;
      report.restoreError = error.message;
      process.exitCode = 1;
    }
  }
  closing = true;
  await browser.close();
  await writeFile(path.join(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report));
}
