import fs from 'node:fs';
import path from 'node:path';
import {createRequire} from 'node:module';
import {pathToFileURL} from 'node:url';
import {parseArgs} from 'node:util';
import {isReadOnlyBatch} from './lib/read-only-device-requests.mjs';
import {EXPECTED_CARDS, createReport, CoverageTracker, recordError, recordHttpStatus, validateSnapshot,
  finishReport, installSseEvidence, recordSseFailures} from './lib/soak-evidence.mjs';

const reportPublishWaitBuffer = new Int32Array(new SharedArrayBuffer(4));

// Retry only transient Windows atomic replacement failures; keep the previous report intact.
export function publishBrowserSoakReport(out, report, io = {}) {
  const write = io.writeFile || fs.writeFileSync, rename = io.rename || fs.renameSync;
  const wait = io.wait || (ms => Atomics.wait(reportPublishWaitBuffer, 0, 0, ms));
  const platform = io.platform || process.platform;
  const pending = path.join(out, 'report.pending'), target = path.join(out, 'report.json');
  report.reportPublishRetries ??= 0;
  report.reportPublishFailures ??= 0;
  report.reportPublishErrorCounts ??= {};
  if (report.reportPublishFailures) report.status = report.stabilityStatus = 'FAIL';
  write(pending, JSON.stringify(report, null, 2));
  for (let attempt = 0; ; attempt++) {
    try { rename(pending, target); return; }
    catch (error) {
      const code = error.code || 'UNKNOWN';
      report.reportPublishErrorCounts[code] = (report.reportPublishErrorCounts[code] || 0) + 1;
      const retry = platform === 'win32' && ['EPERM', 'EACCES', 'EBUSY'].includes(code) && attempt < 3;
      if (retry) report.reportPublishRetries++;
      else { report.reportPublishFailures++; report.status = report.stabilityStatus = 'FAIL'; }
      write(pending, JSON.stringify(report, null, 2));
      if (!retry) throw error;
      wait(50);
    }
  }
}

export function requestAllowed(method, url, body, contentType, loginAllowed) {
  const target = new URL(url), p = target.pathname;
  const unsafe = value => /^\/api\/(?:modbus\/(?:coil\/(?:control|batch|delay)|register\/(?:write|batch-write)|motor\/control|device\/(?:address|baudrate))|periph-exec\/run|system\/(?:reboot|restart|reset)|auth\/(?:login|logout))/.test(new URL(value, url).pathname);
  if (p === '/api/auth/login') return method === 'POST' && loginAllowed;
  if (isReadOnlyBatch(method, url, body, contentType)) {
    return JSON.parse(body).requests.every(item => !unsafe(item.url));
  }
  return ['GET', 'HEAD'].includes(method) && !unsafe(url);
}

// Readiness is structural. Invalid measurements must reach the sampling loop
// and remain failures there, instead of ending observation at startup.
export function dashboardStructureReady(expectedIds) {
  const cards = Array.from(document.querySelectorAll('.dc-monitor-card'));
  const coils = Array.from(document.querySelectorAll('.dc-coil-card'));
  const ids = new Set(cards.map(card => card.dataset.id));
  const channels = new Set(coils.map(coil => coil.dataset.ch));
  return cards.length === expectedIds.length && ids.size === expectedIds.length &&
    expectedIds.every(id => ids.has(id)) && cards.every(card => card.querySelector('.dc-monitor-value')) &&
    coils.length === 4 && channels.size === 4 && ['0', '1', '2', '3'].every(ch => channels.has(ch)) &&
    coils.every(coil => coil.querySelector('.dc-coil-st'));
}

export async function runBrowserSoak(args, dependencies = {}) {
  const seconds = Number(args.seconds ?? 28800), base = args.url;
  const norm = value => String(value || '').replace(/[^0-9a-f]/gi, '').toLowerCase();
  if (!args.out || !base || !/^https?:$/.test(new URL(base).protocol) ||
      norm(args['expected-mac']).length !== 12 || !Number.isInteger(seconds) || seconds < 30 || seconds > 172800) {
    throw Error('Invalid browser soak arguments');
  }
  const output = path.resolve(args.out);
  fs.mkdirSync(path.dirname(output), {recursive: true});
  fs.mkdirSync(output); // Refuse existing evidence, including a concurrent launch.
  const now = dependencies.now || (() => performance.now() / 1000), started = now();
  const report = Object.assign(createReport(seconds), {schemaVersion: 2, pid: process.pid,
    startedAt: new Date().toISOString(), plannedEndAt: new Date(Date.now() + seconds * 1000).toISOString()});
  report.authenticationEvents = [];
  let browser, page, coverage, observing = false, loginSubmitted = false, loginUsed = false;
  let stopped = false, completed = false, terminalFailure = false, closing = false, wake;
  const lastHttpData = {}, pending = new Set();
  const sleep = dependencies.sleep || (delay => new Promise(resolve => {
    const timer = setTimeout(() => { wake = null; resolve(); }, delay * 1000);
    wake = () => { clearTimeout(timer); wake = null; resolve(); };
  }));
  const stop = () => { stopped = true; wake?.(); };
  process.on('SIGINT', stop); process.on('SIGTERM', stop);
  function save() {
    if (coverage && observing) coverage.update(now());
    report.updatedAt = new Date().toISOString(); report.elapsedSeconds = now() - started;
    publishBrowserSoakReport(output, report, dependencies.reportIO);
  }
  function example(kind, value) {
    const event = recordError(report, kind, value);
    fs.appendFileSync(path.join(output, 'events.jsonl'), JSON.stringify(event) + '\n');
  }
  function observerClosed(reason) {
    if (closing || terminalFailure) return;
    terminalFailure = true;
    report.error = reason; report.terminationReason = 'BROWSER_UNAVAILABLE';
    report.observerLostAt = new Date().toISOString(); report.stabilityStatus = 'FAIL';
    example('observerClosed', reason); save(); wake?.();
  }
  async function waitForDashboard() {
    await page.waitForFunction(dashboardStructureReady, EXPECTED_CARDS, {timeout: 45000});
  }
  async function snapshot() {
    const row = await page.evaluate(() => ({at: new Date().toISOString(),
      sseConnections: (window.__soakSseConnections || []).map(info => ({...info,
        aliveEventSeconds: Math.max(0, ((info.lastEventAt ?? info.openedAt ?? info.createdAt) - (info.openedAt ?? info.createdAt)) / 1000)})),
      appVisible: !!document.querySelector('#app-container')?.getClientRects().length,
      sseAges: Object.fromEntries(Object.entries(window.__soakDataAt || {}).map(([k, v]) => [k, (performance.now() - v) / 1000])),
      cards: Array.from(document.querySelectorAll('.dc-monitor-card')).map(e => ({id: e.dataset.id, value: e.querySelector('.dc-monitor-value')?.textContent,
        measurementValid: e.dataset.measurementValid, valuePlaceholder: e.dataset.valuePlaceholder, quality: e.dataset.quality,
        statusText: e.querySelector('.dc-monitor-status:not([hidden])')?.textContent})),
      coils: Array.from(document.querySelectorAll('.dc-coil-card')).map(e => ({channel: e.dataset.ch, text: e.querySelector('.dc-coil-st')?.textContent, unknown: e.classList.contains('dc-coil-unknown')}))}));
    const delta = recordSseFailures(report, row.sseConnections);
    if (delta.streamErrors) example('sseStream', {newErrors: delta.streamErrors, total: report.sseStreamErrors});
    if (delta.jsonErrors) example('sseJson', {newErrors: delta.jsonErrors, total: report.sseJsonErrors});
    return row;
  }
  function fresh(row) {
    return ['modbus-data', 'sensor-data'].every(group => {
      const age = Math.min(row.sseAges?.[group] ?? Infinity,
        lastHttpData[group] === undefined ? Infinity : now() - lastHttpData[group]);
      return age >= 0 && age <= 180;
    });
  }
  save();
  try {
    const driver = dependencies.chromium || createRequire(import.meta.url)('../test/browser/node_modules/@playwright/test').chromium;
    browser = await driver.launch({headless: true});
    browser.on?.('disconnected', () => observerClosed('Browser disconnected'));
    const context = await browser.newContext({viewport: {width: 1920, height: 1200}, serviceWorkers: 'block'});
    await context.addInitScript(installSseEvidence); page = await context.newPage(); page.setDefaultTimeout(45000);
    page.on('close', () => observerClosed('Observation page closed'));
    page.on('crash', () => observerClosed('Observation page crashed'));
    await context.route('**/api/**', async route => {
      const request = route.request(), p = new URL(request.url()).pathname;
      if (!requestAllowed(request.method(), request.url(), request.postData() || '', request.headers()['content-type'] || '', loginSubmitted && !loginUsed)) {
        report.blockedWrites++; report.stabilityStatus = 'FAIL'; example('blockedWrite', p); save();
        await route.abort('blockedbyclient'); return;
      }
      if (p === '/api/auth/login') loginUsed = true;
      await route.continue();
    });
    page.on('pageerror', error => { report.pageErrorCount++; report.stabilityStatus = 'FAIL'; example('pageError', error.message); save(); });
    page.on('response', response => {
      const url = response.url(), p = new URL(url).pathname, status = response.status();
      if (['/api/auth/login', '/api/auth/session'].includes(p)) {
        const event = {at: new Date().toISOString(), path: p, status};
        report.authenticationEvents.push(event);
        const task = response.json().then(body => {
          event.success = body?.success === true;
          if (p === '/api/auth/session') event.sessionValid = body?.data?.sessionValid === true;
        }).catch(() => { event.invalidJson = true; });
        pending.add(task); void task.finally(() => pending.delete(task));
      }
      if (recordHttpStatus(report, status, url, loginSubmitted)) { example('http', {path: p, status}); save(); }
      const group = p === '/api/modbus/status' ? 'modbus-data' : p === '/api/peripherals/read' ? 'sensor-data' : null;
      if (group && status === 200) {
        const task = response.json().then(body => { if (body?.success === true && body.data && Object.keys(body.data).length) lastHttpData[group] = now(); })
          .catch(error => { report.displayErrors++; report.stabilityStatus = 'FAIL'; example('invalidDataResponse', error.message); });
        pending.add(task); void task.finally(() => pending.delete(task));
      }
    });
    page.on('requestfailed', request => {
      const error = request.failure()?.errorText;
      if (loginSubmitted && error && !error.includes('ERR_ABORTED')) {
        report.httpErrorCount++; report.transportErrors++; report.stabilityStatus = 'FAIL';
        example('transport', {path: new URL(request.url()).pathname, error}); save();
      }
    });
    await page.goto(base, {waitUntil: 'domcontentloaded'});
    await page.waitForFunction(() => window.__fastbeeBootReady === true);
    await page.locator('#username').fill(process.env.FASTBEE_USERNAME || 'admin');
    await page.locator('#password').fill(process.env.FASTBEE_PASSWORD);
    loginSubmitted = true; await page.locator('#login-button').click();
    await page.locator('#app-container').waitFor({state: 'visible'});
    const identity = await context.request.get(base + '/api/system/info', {timeout: 15000, maxRedirects: 0});
    if (recordHttpStatus(report, identity.status(), base + '/api/system/info', true)) example('http', {path: '/api/system/info', status: identity.status()});
    const body = await identity.json();
    if (!identity.ok() || body.success !== true || norm(body.data?.network?.macAddress) !== norm(args['expected-mac'])) {
      report.identityErrors++; report.stabilityStatus = 'FAIL'; throw Error('Device identity mismatch or unavailable');
    }
    report.expectedMac = args['expected-mac']; report.identityVerifiedAt = new Date().toISOString();
    await page.locator('.menu-item[data-page="device-control"]').click(); await waitForDashboard();
    const freshDeadline = now() + 45;
    while (!fresh(await snapshot())) {
      if (stopped || report.authenticationLost || now() >= freshDeadline) throw Error('Dashboard data freshness unavailable');
      await sleep(.2);
    }
    report.firstReadyAt = new Date().toISOString(); const observationStarted = now();
    let deadline = observationStarted + seconds, sampleWindowStarted = false;
    report.plannedEndAt = new Date(Date.now() + seconds * 1000).toISOString();
    coverage = new CoverageTracker(report, observationStarted); observing = true;
    let nextNavigation = observationStarted + 600;
    while (!stopped && !terminalFailure) {
      const tick = now(); coverage.attempt(tick);
      let row = {at: new Date().toISOString(), complete: false};
      let sampledAt;
      try {
        if (report.authenticationLost) throw Error('Authentication lost; cached DOM is not a successful sample');
        if (tick >= nextNavigation) {
          nextNavigation = tick + 600; // One navigation attempt per period, even on failure.
          await page.locator('.menu-item[data-page="peripheral"]').click();
          await page.locator('#peripheral-table-body').waitFor({state: 'visible'});
          await page.locator('.menu-item[data-page="device-control"]').click(); await waitForDashboard();
        }
        row = await snapshot(); sampledAt = now(); await Promise.allSettled([...pending]);
        Object.assign(row, validateSnapshot(row, {authenticated: !report.authenticationLost && row.appVisible, fresh: fresh(row)}));
        if (!row.complete) { report.displayErrors++; report.stabilityStatus = 'FAIL'; example('display', row); }
        if (!row.measurementValid) { report.invalidMeasurementSamples++; report.measurementStatus = 'FAIL'; }
        if (row.complete && (report.samples === 0 || tick >= deadline)) await page.screenshot({path: path.join(output, report.samples === 0 ? 'initial.png' : 'final.png'), fullPage: true});
      } catch (error) {
        row.complete = false; row.error = error.message; report.displayErrors++; report.stabilityStatus = 'FAIL'; example('sampling', error.message);
        if (page.isClosed?.() || browser.isConnected?.() === false ||
            /Target page, context or browser has been closed|Page crashed|Browser closed|Browser disconnected/i.test(error.message)) {
          observerClosed(error.message);
        }
      }
      row.completedAt = new Date().toISOString();
      // Screenshot/export/cleanup time cannot extend the measured data span.
      coverage.complete(sampledAt ?? now(), row.at, row.complete);
      if (!sampleWindowStarted && coverage.firstSuccess !== null) {
        sampleWindowStarted = true;
        deadline = Math.max(deadline, coverage.firstSuccess + seconds);
        report.plannedEndAt = new Date(Date.now() + Math.max(0, deadline - now()) * 1000).toISOString();
      }
      fs.appendFileSync(path.join(output, 'samples.jsonl'), JSON.stringify(row) + '\n'); save();
      if (terminalFailure) break;
      if (tick >= deadline) { completed = true; break; }
      await sleep(Math.max(0, Math.min(60 - (now() - tick), deadline - now())));
    }
  } catch (error) {
    report.error = error.message; example('fatal', error.message);
    if (page) try {
      report.startupState = await page.evaluate(() => ({
        bootReady: window.__fastbeeBootReady === true,
        appVisible: !!document.querySelector('#app-container')?.getClientRects().length,
        loginVisible: !!document.querySelector('#login-page')?.getClientRects().length,
        loginButtonDisabled: !!document.querySelector('#login-button')?.disabled
      }));
      await page.screenshot({path: path.join(output, 'failure.png'), fullPage: true});
    } catch (_) { report.startupStateUnavailable = true; }
  }
  finally {
    closing = true;
    if (coverage) coverage.update(now()); observing = false;
    if (page) for (const event of ['response', 'requestfailed', 'pageerror', 'close', 'crash']) page.removeAllListeners(event);
    await Promise.allSettled([...pending]);
    try { if (browser) await browser.close(); } catch (error) { report.pageErrorCount++; example('cleanup', error.message); }
    finishReport(report, {completed, interrupted: stopped});
    if (stopped) report.interruptedAt = new Date().toISOString();
    else if (completed) report.completedAt = new Date().toISOString();
    else report.terminatedAt = new Date().toISOString();
    process.removeListener('SIGINT', stop); process.removeListener('SIGTERM', stop); save();
  }
  return report;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  const {values: args} = parseArgs({options: {url: {type: 'string'}, 'expected-mac': {type: 'string'},
    seconds: {type: 'string', default: '28800'}, out: {type: 'string'}}});
  const report = await runBrowserSoak(args); console.log(JSON.stringify(report)); process.exitCode = report.status === 'PASS' ? 0 : 2;
}
