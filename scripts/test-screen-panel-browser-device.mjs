import { createRequire } from 'node:module';
import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
import { isReadOnlyBatch, isReadOnlyMotorStatus } from './lib/read-only-device-requests.mjs';
import { waitForMainShellStyles } from './lib/browser-main-style-check.mjs';

const SYSTEM_TYPES = { brightness: 'slider', 'sleep-time': 'select', 'touch-beep': 'switch',
  'confirm-idle': 'select', sleep: 'action', buzzer: 'action', restart: 'action' };
function validateSavedLayout(saved) {
  assert.ok(Number.isInteger(saved?.revision), 'Saved revision missing');
  assert.ok(Array.isArray(saved.config?.pages) && saved.config.pages.length > 0, 'At least one saved page is required');
  assert.ok(Number.isInteger(saved.capabilities?.maxPages) && saved.config.pages.length <= saved.capabilities.maxPages,
    'Saved page count exceeds reported capability');
  const ids = saved.config.pages.map(item => item.id);
  assert.equal(new Set(ids).size, ids.length, 'Page identifiers must be unique');
  assert.ok(ids.every(id => typeof id === 'string' && /^[A-Za-z0-9_-]+$/.test(id)), 'Invalid page identifier');
  assert.ok(ids.includes(saved.config.settings?.homePageId), 'Configured home page missing');
  for (const item of saved.config.pages) assert.ok(Array.isArray(item.cards), 'Saved page cards missing');
  return ids;
}
function expectedFooterIds(config) {
  const ids = [config.settings.homePageId, ...(config.settings.bottomNavPageIds || []), ...config.pages.map(item => item.id)];
  const available = new Set(config.pages.map(item => item.id));
  const ordered = [...new Set(ids)].filter(id => available.has(id));
  return config.pages.length > 5 ? [...ordered.slice(0, 4), '__more__'] : ordered.slice(0, 5);
}
function validateConfiguredSystemControls(saved) {
  const actions = saved.config.pages.flatMap(item => item.cards).filter(card => card.kind === 'system');
  assert.ok(actions.length > 0, 'No system controls are configured for --verify-system');
  assert.ok(Array.isArray(saved.capabilities.systemActions), 'System capability list missing');
  for (const card of actions) {
    assert.ok(Object.hasOwn(SYSTEM_TYPES, card.systemActionId), 'Unknown system action');
    assert.equal(card.visualType, SYSTEM_TYPES[card.systemActionId], 'System control visual type mismatch');
    if (card.systemActionId === 'brightness') {
      assert.ok(Number.isInteger(card.actionValue) && card.actionValue >= 0 && card.actionValue <= 100, 'Brightness must be 0..100');
    }
    if (card.systemActionId === 'restart') assert.equal(card.requireConfirmation, true, 'Restart must require confirmation');
  }
  return { configured: actions.map(card => card.systemActionId),
    unavailable: [...new Set(actions.map(card => card.systemActionId))].filter(action => !saved.capabilities.systemActions.includes(action)) };
}
function isFeatureToggle(method, url, body, contentType = '') {
  let target, payload;
  try { target = new URL(url); payload = JSON.parse(body); } catch { return false; }
  return method === 'POST' && target.pathname === '/api/features/config' && !target.search &&
    contentType.split(';')[0].trim().toLowerCase() === 'application/json' &&
    payload && Object.keys(payload).join(',') === 'features' && payload.features &&
    Object.keys(payload.features).join(',') === 'screenPanel' && payload.features.screenPanel &&
    Object.keys(payload.features.screenPanel).join(',') === 'enabled' &&
    typeof payload.features.screenPanel.enabled === 'boolean';
}
function requestAllowed(method, url, body, contentType, allowFeatureMutation, origin) {
  const target = new URL(url);
  if (target.origin !== origin) return false;
  return ['GET', 'HEAD', 'OPTIONS'].includes(method) ||
    (method === 'POST' && ['/api/auth/login', '/api/auth/logout'].includes(target.pathname) && !target.search) ||
    (allowFeatureMutation && isFeatureToggle(method, url, body, contentType)) ||
    isReadOnlyBatch(method, url, body, contentType) || isReadOnlyMotorStatus(method, url, body, contentType);
}
function isIntentionalBatchNavigationAbort(request, failure, generation, navigation, now, origin) {
  if (failure !== 'net::ERR_ABORTED' || !navigation || now < navigation.at || now > navigation.until ||
      !Number.isInteger(generation) || generation >= navigation.generation ||
      !['fetch', 'xhr'].includes(request.resourceType())) return false;
  try {
    return new URL(request.url()).origin === origin && isReadOnlyBatch(
      request.method(), request.url(), request.postData(), request.headers()['content-type']);
  } catch { return false; }
}
if (process.argv.includes('--contract-only')) {
  for (const count of [1, 2, 5, 6]) {
    const config = { pages: Array.from({ length: count }, (_, index) => ({ id: 'page' + index, cards: [] })),
      settings: { homePageId: 'page0', bottomNavPageIds: [] } };
    const saved = { revision: 1, config, capabilities: { maxPages: 8, systemActions: ['brightness', 'sleep'] } };
    assert.equal(validateSavedLayout(saved).length, count);
    assert.deepEqual(expectedFooterIds(config), count <= 5 ? config.pages.map(item => item.id) : ['page0', 'page1', 'page2', 'page3', '__more__']);
    config.pages[0].cards.push({ kind: 'system', systemActionId: 'brightness', visualType: 'slider', actionValue: 0 });
    assert.deepEqual(validateConfiguredSystemControls(saved).configured, ['brightness']);
    config.pages[0].cards[0].visualType = 'action';
    assert.throws(() => validateConfiguredSystemControls(saved));
    config.pages[0].cards[0] = { kind: 'system', systemActionId: 'restart', visualType: 'action', requireConfirmation: false };
    assert.throws(() => validateConfiguredSystemControls(saved));
    config.pages[0].cards = []; config.settings.homePageId = 'missing';
    assert.throws(() => validateSavedLayout(saved));
  }
  const origin = 'http://localhost';
  const allowed = (route, body, mutation = false) => requestAllowed('POST', origin + route, JSON.stringify(body), 'application/json', mutation, origin);
  assert.ok(allowed('/api/auth/login', { username: 'contract' }));
  assert.ok(allowed('/api/modbus/motor/control', { action: 'readStatus', slaveAddress: 8 }));
  assert.ok(allowed('/api/batch', { requests: [{ url: '/api/system/status' }] }));
  for (const route of ['/api/screen-panel/config', '/api/screen-panel/weather', '/api/screen-panel/weather/refresh', '/api/periph-exec/run'])
    assert.equal(allowed(route, {}), false);
  assert.equal(allowed('/api/modbus/motor/control', { action: 'forward', slaveAddress: 8 }), false);
  const toggle = { features: { screenPanel: { enabled: true } } };
  assert.equal(allowed('/api/features/config', toggle), false);
  assert.equal(allowed('/api/features/config', toggle, true), true);
  assert.equal(allowed('/api/features/config', { ...toggle, reset: true }, true), false);
  assert.equal(requestAllowed('GET', 'http://example.invalid/api/health', undefined, '', false, origin), false);
  const batchRequest = { method: () => 'POST', url: () => origin + '/api/batch', resourceType: () => 'fetch',
    postData: () => JSON.stringify({ requests: [{ url: '/api/system/status', method: 'GET' }] }),
    headers: () => ({ 'content-type': 'application/json' }) };
  const navigation = { at: 1000, until: 2500, generation: 1 };
  const cancelled = (request = batchRequest, failure = 'net::ERR_ABORTED', generation = 0, now = 1100, nav = navigation) =>
    isIntentionalBatchNavigationAbort(request, failure, generation, nav, now, origin);
  assert.equal(cancelled(), true);
  assert.equal(cancelled(batchRequest, 'net::ERR_TIMED_OUT'), false);
  assert.equal(cancelled(batchRequest, 'net::ERR_ABORTED', 1), false);
  assert.equal(cancelled(batchRequest, 'net::ERR_ABORTED', null), false);
  assert.equal(cancelled(batchRequest, 'net::ERR_ABORTED', 0, 999), false);
  assert.equal(cancelled(batchRequest, 'net::ERR_ABORTED', 0, 2501), false);
  assert.equal(cancelled(batchRequest, 'net::ERR_ABORTED', 0, 1100, null), false);
  assert.equal(cancelled({ ...batchRequest, url: () => 'http://example.invalid/api/batch' }), false);
  assert.equal(cancelled({ ...batchRequest, url: () => origin + '/api/batch?write=1' }), false);
  assert.equal(cancelled({ ...batchRequest, resourceType: () => 'document' }), false);
  assert.equal(cancelled({ ...batchRequest, postData: () => JSON.stringify({ requests: [{ url: '/api/system/status', method: 'POST' }] }) }), false);
  const html = await readFile(new URL('../web-src/pages/screen-panel.html', import.meta.url), 'utf8');
  assert.ok(!html.includes('id="sp-preview"') && !html.includes('id="sp-tab-config"'), 'No duplicate preview/editor tabs');
  assert.ok(html.includes('id="sp-editor-canvas"') && html.includes('id="sp-component-properties"'), 'Unified workspace and component properties required');
  assert.equal((html.match(/id="sp-save-config"/g) || []).length, 1, 'Unified save action must be unique');
  console.log('PASS: screen-panel browser contract (dynamic navigation, unified live workspace, system controls, write guards)');
  process.exit(0);
}
const require = createRequire(import.meta.url);
const { chromium } = require('../test/browser/node_modules/playwright');
const argument = (key, fallback) => process.argv.find((value) => value.startsWith(`--${key}=`))?.slice(key.length + 3) || fallback;
const requestedUrl = argument('url');
assert.ok(requestedUrl, 'Use --url=http://device [--output=directory] [--verify-weather] [--verify-system]');
const target = new URL(requestedUrl);
assert.ok(['http:', 'https:'].includes(target.protocol) && !target.username && !target.password, 'Use an HTTP(S) origin and environment credentials');
const baseUrl = target.origin;
const output = path.resolve(argument('output', 'artifacts/screen-panel-browser-device'));
await mkdir(output, { recursive: true });
const report = { baseUrl, at: new Date().toISOString(), passed: false, panelPreviewTested: false, mutations: [], readOnlyPosts: [], pageErrors: [], httpErrors: [], loginRequests: 0, requestFailures: [], intentionalNavigationAborts: [] };
let browser;
const exerciseSwitch = process.argv.includes('--exercise-switch');
const restoreOnly = process.argv.includes('--restore-only');
const allowFeatureMutation = exerciseSwitch || restoreOnly;
let page;
let restoreSwitch = false;
let closing = false;
let pageGeneration = 0;
let navigation;
const requestGenerations = new WeakMap();
function beginMenuNavigation(targetPage) {
  const at = Date.now();
  navigation = { at, until: at + 1500, generation: ++pageGeneration, targetPage };
}
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
  browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, serviceWorkers: 'block' });
  page = await context.newPage();
  const isStatusRead = request => isReadOnlyMotorStatus(request.method(), request.url(), request.postData(), request.headers()['content-type']);
  const isBatchRead = request => isReadOnlyBatch(request.method(), request.url(), request.postData(), request.headers()['content-type']);
  await page.route('**/api/**', async route => {
    const request = route.request();
    if (requestAllowed(request.method(), request.url(), request.postData(), request.headers()['content-type'], allowFeatureMutation, baseUrl)) await route.continue();
    else await route.abort('blockedbyclient');
  });
  page.setDefaultTimeout(30_000);
  page.on('pageerror', (error) => report.pageErrors.push(error.message));
  page.on('requestfailed', request => {
    if (closing) return;
    const failure = request.failure()?.errorText || '';
    const generation = requestGenerations.get(request);
    const at = Date.now();
    if (isIntentionalBatchNavigationAbort(request, failure, generation, navigation, at, baseUrl)) {
      report.intentionalNavigationAborts.push({ route: new URL(request.url()).pathname, method: request.method(),
        resourceType: request.resourceType(), requestGeneration: generation, navigationGeneration: navigation.generation,
        navigationTarget: navigation.targetPage, millisecondsAfterNavigation: at - navigation.at });
      return;
    }
    report.requestFailures.push({
      route: new URL(request.url()).pathname, method: request.method(), error: request.failure()?.errorText,
    });
  });
  page.on('response', response => {
    if (response.status() >= 400) report.httpErrors.push({
      route: new URL(response.url()).pathname, method: response.request().method(), status: response.status(),
    });
  });
  page.on('request', (request) => {
    requestGenerations.set(request, pageGeneration);
    const route = new URL(request.url()).pathname;
    if (request.method() === 'POST' && route === '/api/auth/login') ++report.loginRequests;
    if (isStatusRead(request)) report.readOnlyPosts.push({ method: request.method(), route, action: 'readStatus' });
    else if (isBatchRead(request)) report.readOnlyPosts.push({ method: request.method(), route, action: 'batchGet' });
    if (!['GET', 'HEAD', 'OPTIONS'].includes(request.method())) {
      const login = request.method() === 'POST' && ['/api/auth/login', '/api/auth/logout'].includes(route);
      const featureExercise = allowFeatureMutation && isFeatureToggle(request.method(), request.url(), request.postData(), request.headers()['content-type']);
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
      const response = await fetch('/api/screen-panel/config', { cache: 'no-store', headers: window.AppState.getAuthHeader(), signal: AbortSignal.timeout(15_000) });
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
    beginMenuNavigation('screen-panel');
    await page.locator('.menu-item[data-page="screen-panel"]').click();
    await page.locator('#screen-panel-page.active').waitFor({ state: 'visible' });
    await page.waitForFunction(() => window.AppState?._spModel?.loaded === true);
    const savedLayout = await page.evaluate(async () => {
      const response = await fetch('/api/screen-panel/config', {
        headers: window.AppState.getAuthHeader(), signal: AbortSignal.timeout(15000),
      });
      if (!response.ok) throw new Error(`Panel config HTTP ${response.status}`);
      const body = await response.json();
      if (body.success !== true) throw new Error('Panel config response failed');
      return body.data;
    });
    const savedPageIds = validateSavedLayout(savedLayout);
    const savedPageCount = savedPageIds.length;
    report.savedPageCount = savedPageCount;
    assert.equal(await page.locator('.sp-top-tabs').count(), 0);
    assert.deepEqual((await page.locator('.sp-property-tabs > button').allTextContents()).map(text => text.trim()), ['页面属性', '组件属性', '全局属性']);
    await page.locator('#sp-editor-canvas').waitFor({ state: 'visible' });
    assert.equal(await page.locator('#sp-save-config').isVisible(), true, 'Unified workspace has one save action');
    const footer = page.locator('#sp-editor-canvas .sp-device-footer > button');
    assert.deepEqual(await footer.evaluateAll(nodes => nodes.map(node => node.dataset.spEditorPage)), expectedFooterIds(savedLayout.config));
    const openPreviewPage = async id => {
      let button = page.locator(`#sp-editor-canvas .sp-device-footer > [data-sp-editor-page="${id}"]`);
      if (await button.count() === 0) {
        await page.locator('#sp-editor-canvas [data-sp-editor-page="__more__"]').click();
        const more = page.locator('#sp-editor-canvas .sp-nav-more [data-sp-editor-page]');
        assert.deepEqual(await more.evaluateAll(nodes => nodes.map(node => node.dataset.spEditorPage)), savedPageIds);
        button = page.locator(`#sp-editor-canvas .sp-nav-more [data-sp-editor-page="${id}"]`);
      }
      await button.click();
      const savedPage = savedLayout.config.pages.find(item => item.id === id);
      // Published panel properties are mangled; verify the visible page contract.
      await page.waitForFunction(({ pageId, cardIds }) => {
        const surface = document.getElementById('sp-editor-canvas');
        if (!surface) return false;
        const entry = [...surface.querySelectorAll('.sp-device-footer > button')]
          .find(node => node.dataset.spEditorPage === pageId);
        const renderedIds = [...surface.querySelectorAll('.sp-panel-card')].map(node => node.dataset.cardId);
        return (!entry || entry.getAttribute('aria-current') === 'page') &&
          JSON.stringify(renderedIds) === JSON.stringify(cardIds);
      }, { pageId: id, cardIds: savedPage.cards.map(card => card.id) });
      const cards = page.locator('#sp-editor-canvas .sp-panel-card');
      assert.deepEqual(await cards.evaluateAll(nodes => nodes.map(node => node.dataset.cardId)), savedPage.cards.map(card => card.id));
      assert.equal(await page.locator('#sp-editor-canvas .sp-panel-card button:not([disabled]):not([data-sp-resize]), #sp-editor-canvas .sp-panel-card input:not([disabled])').count(), 0,
        'Saved preview controls must never dispatch hardware actions');
    };
    for (const id of savedPageIds) await openPreviewPage(id);
    report.previewPageIds = savedPageIds;
    if (process.argv.includes('--verify-panel-layout')) {
      report.motorRows = [];
      for (const savedPage of savedLayout.config.pages) {
        const motors = savedPage.cards.filter(card => card.visualType === 'motor');
        if (!motors.length) continue;
        await openPreviewPage(savedPage.id);
        for (const width of [1440, 390]) {
          await page.setViewportSize({ width, height: 900 });
          // Container-query units settle on the next rendered frames after resize.
          await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
          for (const card of motors) {
            const buttons = page.locator(`#sp-editor-canvas [data-card-id="${card.id}"] .sp-motor-actions button`);
            assert.deepEqual(await buttons.allTextContents(), ['正转', '停止', '反转']);
            const boxes = await buttons.evaluateAll(nodes => nodes.map(node => {
              const r = node.getBoundingClientRect();
              return { x: r.x, y: r.y, width: r.width, height: r.height, disabled: node.disabled, fits: node.scrollWidth <= node.clientWidth };
            }));
            report.motorRows.push({ pageId: savedPage.id, cardId: card.id, width, boxes });
            assert.ok(boxes.every(box => box.disabled && box.fits && box.width > 0 && box.height > 0));
            for (let index = 1; index < boxes.length; index++) {
              assert.ok(Math.abs(boxes[index].y - boxes[0].y) < 1);
              assert.ok(Math.abs(boxes[index].width - boxes[0].width) < 1);
              assert.ok(boxes[index].x > boxes[index-1].x + boxes[index-1].width);
            }
            assert.equal(await buttons.nth(1).getAttribute('class'), 'sp-motor-stop');
          }
          await page.locator('#sp-editor-frame').screenshot({ path: path.join(output, `motor-row-${savedPage.id}-${width}.png`) });
        }
      }
      assert.ok(report.motorRows.length > 0, 'Configured motor required for --verify-panel-layout');
      await page.setViewportSize({ width: 1440, height: 900 });
      await page.locator('#sp-global-settings').click();
      const toggle = page.locator('#sp-auto-sleep-enabled');
      assert.equal(await toggle.isChecked(), savedLayout.config.settings.idleSleepSeconds > 0);
      assert.equal(Number(await page.locator('#sp-idle-sleep').inputValue()), savedLayout.config.settings.idleSleepSeconds);
      report.autoSleepSwitchMatchesSavedConfiguration = true;
      await page.locator('#sp-properties-toggle').click();
      assert.equal(await page.locator('#sp-page-tabs .sp-sort-handle').count(), savedPageCount);
      report.pageOrderHandlesVisible = true;
    }
    if (process.argv.includes('--verify-charts')) {
      report.lineCharts = [];
      for (const savedPage of savedLayout.config.pages) {
        const charts = savedPage.cards.filter(card => card.kind === 'chart' && card.visualType === 'line' && card.sourceRef);
        if (!charts.length) continue;
        await openPreviewPage(savedPage.id);
        for (const card of charts) {
          const surface = page.locator('#sp-editor-canvas .sp-panel-card').filter({ has: page.locator('.sp-line-chart') });
          await page.waitForFunction(cardId => {
            const cardNode = [...document.querySelectorAll('#sp-editor-canvas .sp-panel-card')]
              .find(node => node.dataset.cardId === cardId);
            return cardNode?.querySelectorAll('.sp-line-chart polyline').length > 0;
          }, card.id, { timeout: 45000 });
          const drawing = await surface.evaluateAll((nodes, cardId) => {
            const node = nodes.find(item => item.dataset.cardId === cardId);
            const segments = [...node.querySelectorAll('.sp-line-chart polyline')];
            return {
              dots: node.querySelectorAll('.sp-line-chart circle').length,
              segments: segments.length,
              visibleSegments: segments.filter(segment => {
                const style = getComputedStyle(segment);
                return style.stroke !== 'none' && Number.parseFloat(style.strokeWidth) > 0 && Number(style.opacity) > 0;
              }).length,
            };
          }, card.id);
          assert.ok(drawing.dots >= 2 && drawing.visibleSegments > 0, 'Live chart requires connected, visible historical samples');
          report.lineCharts.push({ pageId: savedPage.id, cardId: card.id, sourceRef: card.sourceRef, ...drawing });
        }
        await page.screenshot({ path: path.join(output, 'chart-' + savedPage.id + '.png'), fullPage: true });
      }
      assert.ok(report.lineCharts.length > 0, 'A configured line chart is required for --verify-charts');
      report.liveChartSegmentsTested = true;
    }
    if (process.argv.includes('--verify-weather')) {
      const weatherPage = savedLayout.config.pages.findIndex(item =>
        item.cards.some(card => card.kind === 'weather' && card.sourceRef));
      assert.ok(weatherPage >= 0, 'A bound weather card is required');
      await openPreviewPage(savedPageIds[weatherPage]);
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
      report.systemControls = validateConfiguredSystemControls(savedLayout);
      for (const systemPage of savedLayout.config.pages.filter(item => item.cards.some(card => card.kind === 'system'))) {
        await openPreviewPage(systemPage.id);
        for (const card of systemPage.cards.filter(item => item.kind === 'system')) {
          const surface = page.locator(`#sp-editor-canvas [data-card-id="${card.id}"]`);
          await surface.waitFor({ state: 'visible' });
          assert.equal(await surface.getAttribute('data-kind'), card.systemActionId);
          if (card.systemActionId === 'brightness') {
            const slider = surface.locator('input[type="range"]');
            assert.equal(await slider.getAttribute('min'), '0');
            assert.equal(await slider.getAttribute('max'), '100');
            assert.equal(Number(await slider.inputValue()), savedLayout.config.settings.brightnessPercent);
            assert.equal(await slider.isDisabled(), true, 'Preview brightness must not alter the device');
          } else {
            assert.ok(await surface.locator('button[disabled]').count() > 0, 'System action preview missing');
          }
        }
      }
      await page.screenshot({ path: path.join(output, 'system.png'), fullPage: true });
      report.systemControlsPreviewTested = true;
    }
    await page.locator('#sp-config').waitFor({ state: 'visible' });
    await page.locator('#sp-editor-canvas').waitFor({ state: 'visible' });
    assert.equal(await page.locator('#sp-save-config').count(), 1, 'Editor must have one unified save action');
    assert.equal(await page.locator('#sp-save-config').isVisible(), true);
    const pageTabs = page.locator('#sp-page-tabs .sp-page-tab');
    assert.deepEqual(await pageTabs.evaluateAll(nodes => nodes.map(node => node.dataset.pageId)), savedPageIds);
    const readOnlyEditor = await page.locator('#sp-config-locked').isVisible();
    if (!readOnlyEditor && savedPageCount < savedLayout.capabilities.maxPages) {
      await page.locator('#sp-page-preset').selectOption('blank');
      page.once('dialog', dialog => dialog.accept('临时验收草稿'));
      await page.locator('#sp-page-add').click();
      assert.equal(await pageTabs.count(), savedPageCount + 1);
      report.unsavedPageAppendTested = true;
    } else {
      report.draftAppendOutcome = readOnlyEditor ? 'skipped-editor-read-only' : 'skipped-page-limit';
    }
    // Draft changes are visible immediately but must never persist without saving.
    await pageTabs.first().click();

    await openPreviewPage(savedPageIds[0]);
    const persistedLayout = await page.evaluate(async () => {
      const response = await fetch('/api/screen-panel/config', {
        headers: window.AppState.getAuthHeader(), signal: AbortSignal.timeout(15000),
      });
      if (!response.ok) throw new Error(`Panel config HTTP ${response.status}`);
      const body = await response.json();
      if (body.success !== true) throw new Error('Panel config readback failed');
      return { revision: body.data.revision, config: body.data.config };
    });
    assert.equal(persistedLayout.revision, savedLayout.revision, 'Draft test must not change revision');
    assert.deepEqual(persistedLayout.config, savedLayout.config, 'Draft test must not persist changes');
    report.configurationUnchanged = true;
    if (report.unsavedPageAppendTested) {
      page.once('dialog', dialog => dialog.accept());
      await page.locator('#sp-reset-draft').click();
      assert.equal(await pageTabs.count(), savedPageCount);
      report.draftDiscarded = true;
    }
    report.panelPreviewTested = true;
  } else {
    assert.equal(await page.locator('.menu-item[data-page="screen-panel"]').isVisible(), false);
    beginMenuNavigation('features');
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
        assert.ok(canvas && toolbox && canvas.y < toolbox.y, 'Mobile editor canvas must precede the component library');
      }
      const editorViewport = await page.evaluate(() => ({ width: innerWidth, document: document.documentElement.scrollWidth }));
      assert.ok(editorViewport.document <= editorViewport.width + 1, `${name} editor horizontal overflow`);
      await page.locator('#sp-runtime-badge').scrollIntoViewIfNeeded();
      await page.screenshot({ path: path.join(output, `${name}-editor.png`), fullPage: true });
        await page.locator('#sp-editor-canvas').waitFor({ state: 'visible' });
      assert.equal(await page.locator('#sp-save-config').isVisible(), true);
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
  if (page && !page.isClosed()) await page.evaluate(async () => {
    if (window.AppState?.getAuthHeader) await fetch('/api/auth/logout', { method: 'POST', headers: window.AppState.getAuthHeader(), signal: AbortSignal.timeout(5000) });
  }).catch(() => {});
  if (browser) await browser.close();
  await writeFile(path.join(output, 'report.json'), JSON.stringify(report, null, 2) + '\n');
  console.log(JSON.stringify(report));
}
