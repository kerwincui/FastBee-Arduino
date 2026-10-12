// Real-device, read-only menu regression. Login and read-only batch POSTs are allowed.
// node scripts/test-web-device-menus.mjs http://192.168.5.54 artifacts/web-device-menus
import { createRequire } from 'node:module';
import fs from 'node:fs';
import path from 'node:path';
import { isIntentionalHealthCancellation } from './lib/menu-cancellation-evidence.mjs';
import { isReadOnlyBatch, isReadOnlyMotorStatus } from './lib/read-only-device-requests.mjs';
import { waitForMainShellStyles } from './lib/browser-main-style-check.mjs';
import { safeObservationError, safeObservationEvents, safeObservationCaptureStats } from './lib/safe-observer-error.mjs';
const require = createRequire(import.meta.url);
const { chromium } = require('../test/browser/node_modules/playwright');
const base = process.argv[2];
const output = path.resolve(process.argv[3] || 'artifacts/web-device-menus');
if (!base || !/^https?:\/\//.test(base)) throw new Error('Provide device HTTP(S) origin');
fs.mkdirSync(output, { recursive: true });
const report = { base, at: new Date().toISOString(), passed: false, mutations: [], readOnlyPosts: [], pageErrors: [], httpErrors: [], requestFailures: [], intentionalCancellations: [], apiIntegrity: [], viewports: [], safeObservationEvents, safeObservationCaptureStats };
const browser = await chromium.launch({ headless: true });
const pendingEvidence = new Set();
let closing = false, generation = 0, sessionLost = false;
try {
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, serviceWorkers: 'block' });
  const page = await context.newPage();
  const isStatusRead = request => isReadOnlyMotorStatus(request.method(), request.url(), request.postData(), request.headers()['content-type']);
  const isBatchRead = request => isReadOnlyBatch(request.method(), request.url(), request.postData(), request.headers()['content-type']);
  // Stop unexpected writes before transmission, while permitting this known
  // read-only legacy POST. Never approve the endpoint without checking action.
  await page.route('**/api/**', async route => {
    const request = route.request();
    const pathname = new URL(request.url()).pathname;
    const login = request.method() === 'POST' && pathname === '/api/auth/login';
    if (['GET', 'HEAD', 'OPTIONS'].includes(request.method()) || login ||
        isBatchRead(request) || isStatusRead(request)) await route.continue();
    else await route.abort('blockedbyclient');
  });
  page.setDefaultTimeout(25000);
  const requestGeneration = new WeakMap();
  page.on('pageerror', error => report.pageErrors.push(safeObservationError(error, 'pageerror')));
  page.on('request', request => {
    requestGeneration.set(request, generation);
    const route = new URL(request.url()).pathname;
    if (isStatusRead(request)) report.readOnlyPosts.push({ method: request.method(), route, action: 'readStatus' });
    else if (isBatchRead(request)) report.readOnlyPosts.push({ method: request.method(), route, action: 'batchGet' });
    else if (!['GET', 'HEAD', 'OPTIONS'].includes(request.method()) &&
             !(request.method() === 'POST' && route === '/api/auth/login'))
      report.mutations.push({ method: request.method(), route });
  });
  page.on('response', response => {
    const route = new URL(response.url()).pathname;
    if (response.status() === 200 && ['/api/peripherals/types', '/api/protocol/config', '/api/features/status'].includes(route)) {
      const row = { route, status: 200, passed: false };
      report.apiIntegrity.push(row);
      const evidence = (async () => {
        const body = await response.json();
        row.success = body.success === true;
        row.passed = row.success && !!body.data && typeof body.data === 'object';
        if (route === '/api/peripherals/types') {
          row.counts = Object.fromEntries(['communication', 'gpio', 'analog', 'special'].map(key => [key, body.data?.[key]?.length || 0]));
          const items = Object.values(body.data || {}).filter(Array.isArray).flat();
          row.itemsComplete = items.length > 0 && items.every(item => Number.isFinite(item.value) && typeof item.name === 'string' && typeof item.implemented === 'boolean');
          row.passed &&= Object.values(row.counts).every(count => count > 0) && row.itemsComplete && !!body.hardware;
        } else if (route === '/api/features/status') {
          row.featureCount = body.data?.features?.length || 0;
          row.passed &&= row.featureCount > 0;
        }
      })().catch(error => { row.error = safeObservationError(error, 'api'); });
      pendingEvidence.add(evidence);
      evidence.finally(() => pendingEvidence.delete(evidence));
      return;
    }
    if (response.status() < 400) return;
    const row = { route, status: response.status() };
    report.httpErrors.push(row);
    if (row.status === 401 && row.route !== '/api/auth/login') sessionLost = true;
    const evidence = (async () => {
      const headers = await response.request().allHeaders();
      // Never save cookie/token values or whole request headers.
      row.hasCookie = !!headers.cookie;
      row.hasAuthorization = !!headers.authorization;
      row.retryAfter = response.headers()['retry-after'] || null;
      const contentType = response.headers()['content-type'] || '';
      if (/json|text\/plain/.test(contentType)) {
        row.errorBody = safeObservationError(await response.text(), 'http-body');
      }
    })().catch(error => { row.evidenceError = safeObservationError(error, 'http-evidence'); });
    pendingEvidence.add(evidence);
    evidence.finally(() => pendingEvidence.delete(evidence));
  });
  page.on('requestfailed', request => {
    if (closing) return;
    const reason = request.failure()?.errorText;
    const route = new URL(request.url()).pathname;
    if (route !== '/api/health' && reason === 'net::ERR_ABORTED' && requestGeneration.get(request) < generation) return;
    report.requestFailures.push({ route, method: request.method(),
      reason: reason === 'net::ERR_ABORTED' ? reason : safeObservationError(reason, 'request'), at: Date.now() });
  });
  const bootStart = Date.now();
  await page.goto(base, { waitUntil: 'domcontentloaded', timeout: 45000 });
  await page.waitForFunction(() => window.__fastbeeBootReady === true, { timeout: 45000 });
  await page.evaluate(() => {
    window.__menuHealthCancellations = [];
    const get = window.apiGetSilent;
    window.apiGetSilent = function(route, ...args) {
      const startedAt = Date.now();
      const startGeneration = window.apiGetGovernorState?.().pageGeneration;
      return get.call(this, route, ...args).catch(error => {
        if (route === '/api/health') window.__menuHealthCancellations.push({
          route, startedAt, finishedAt: Date.now(), startGeneration,
          endGeneration: window.apiGetGovernorState?.().pageGeneration,
          pageAborted: error?._pageAborted === true, requestTimeout: error?._requestTimeout === true
        });
        throw error;
      });
    };
  });
  await page.locator('#username').fill(process.env.FASTBEE_USERNAME || 'admin');
  await page.locator('#password').fill(process.env.FASTBEE_PASSWORD || 'admin123');
  await page.locator('#login-button').click();
  await page.locator('#app-container').waitFor({ state: 'visible', timeout: 45000 });
  report.mainShellStyle = await waitForMainShellStyles(page);
  report.loginMs = Date.now() - bootStart;
  await page.waitForFunction(() => window.AppState?._featureStatusReady === true, { timeout: 30000 });
  const menus = await page.locator('.menu-item[data-page]').evaluateAll(elements => elements.filter(e => getComputedStyle(e).display !== 'none').map(e => e.dataset.page));
  report.enabledMenus = menus;
  for (const [name, width, height] of [['desktop', 1440, 900], ['mobile', 390, 844]]) {
    await page.setViewportSize({ width, height });
    const viewport = { name, width, height, pages: [], mainShellStyle: await waitForMainShellStyles(page) };
    report.viewports.push(viewport);
    for (const menu of menus) {
      if (sessionLost) throw new Error('Protected API returned 401; stopped without relogin or extra traffic');
      const result = { menu, passed: false };
      const started = Date.now();
      try {
        if (width <= 768 && !(await page.locator('#sidebar').evaluate(e => e.classList.contains('expanded'))))
          await page.locator('#sidebar-toggle').click();
        generation++;
        await page.locator(`.menu-item[data-page="${menu}"]`).click();
        await page.waitForFunction(id => {
          const e = document.getElementById(id + '-page');
          if (!e || !e.classList.contains('active') || e.dataset.moduleLoading === 'true' || e.innerText.trim().length < 30) return false;
          const visible = el => !!(el.offsetWidth || el.offsetHeight || el.getClientRects().length);
          return !Array.from(e.querySelectorAll('*')).some(el => visible(el) && (
            (el.children.length === 0 && /^\s*(加载中|正在加载|Loading)(\.|…|\s)*$/i.test(el.textContent)) || el.classList.contains('loading-spinner')));
        }, menu, { timeout: 30000 });
        await page.waitForTimeout(1000);
        result.content = await page.locator('#' + menu + '-page').evaluate(e => ({ textLength: e.innerText.trim().length, controls: e.querySelectorAll('button,input,select,a').length, moduleLoading: e.dataset.moduleLoading === 'true' }));
        result.horizontalOverflow = await page.evaluate(() => Math.max(0, document.documentElement.scrollWidth - innerWidth));
        result.errorNotices = await page.locator('.notification-error:visible,.toast-error:visible,.message-error:visible,.alert-error:visible').count();
        result.passed = result.content.textLength >= 30 && !result.content.moduleLoading && result.horizontalOverflow <= 1 && result.errorNotices === 0;
        await page.screenshot({ path: path.join(output, `${name}-${menu}.png`), fullPage: true });
      } catch (error) { result.error = safeObservationError(error, 'menu'); }
      result.ms = Date.now() - started;
      viewport.pages.push(result);
      fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2));
      console.log(JSON.stringify({ viewport: name, ...result }));
      if (!result.passed) throw new Error(`Menu ${name}/${menu} failed; stopped to avoid piling up requests`);
      await page.waitForTimeout(1500);
    }
  }
  await Promise.allSettled([...pendingEvidence]);
  report.healthCancellationEvidence = await page.evaluate(() => window.__menuHealthCancellations || []);
  report.requestFailures = report.requestFailures.filter(failure => {
    if (!isIntentionalHealthCancellation(failure, report.healthCancellationEvidence)) return true;
    report.intentionalCancellations.push(failure);
    return false;
  });
  report.passed = report.viewports.every(v => v.pages.every(p => p.passed)) && report.apiIntegrity.every(row => row.passed)
    && !report.pageErrors.length && !report.httpErrors.length && !report.mutations.length && !report.requestFailures.length;
} catch (error) { report.error = safeObservationError(error, 'fatal'); }
finally {
  await Promise.allSettled([...pendingEvidence]);
  closing = true;
  await browser.close();
  fs.writeFileSync(path.join(output, 'report.json'), JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report));
  if (!report.passed) process.exitCode = 1;
}
