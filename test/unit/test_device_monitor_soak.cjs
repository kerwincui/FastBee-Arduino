const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {EventEmitter} = require('node:events');
const vm = require('node:vm');
const modules = Promise.all([import('../../scripts/lib/soak-evidence.mjs'), import('../../scripts/test-device-monitor-soak.mjs')]);

test('dashboard readiness accepts rendered unavailable measurements for continued failure sampling', async () => {
  const [evidence, sampler] = await modules;
  const cards = evidence.EXPECTED_CARDS.map(id => ({dataset: {id},
    querySelector: () => ({textContent: '--'})}));
  const coils = ['0', '1', '2', '3'].map(ch => ({dataset: {ch},
    classList: {contains: () => true}, querySelector: () => ({textContent: '--'})}));
  const document = {querySelectorAll: selector => selector === '.dc-monitor-card' ? cards : coils};
  const ready = () => vm.runInNewContext('(' + sampler.dashboardStructureReady.toString() + ')(ids)',
    {document, ids: evidence.EXPECTED_CARDS});
  assert.equal(ready(), true);
  cards[0].dataset.id = 'unexpected'; assert.equal(ready(), false);
  cards[0].dataset.id = evidence.EXPECTED_CARDS[0];
  coils[0].dataset.ch = '1'; assert.equal(ready(), false);
});

function row(ids) {
  return {at: new Date().toISOString(), appVisible: true, sseAges: {'modbus-data': 0, 'sensor-data': 0},
    cards: ids.map(id => ({id, value: id.includes('soil_humidity') ? '--' : '24.3'})),
    coils: ['0', '1', '2', '3'].map(channel => ({channel, text: 'OFF', unknown: false}))};
}

test('successful coverage detects one outage even with regular attempts', async () => {
  const [m] = await modules, r = m.createReport(600), c = new m.CoverageTracker(r, 1000);
  for (let t = 0; t <= 300; t += 60) { c.attempt(1000 + t); c.complete(1000 + t, String(t), t === 0); }
  assert.equal(r.attempts, 6); assert.equal(r.samples, 1); assert.equal(r.maximumAttemptGapSeconds, 60);
  assert.equal(r.maximumSamplingGapSeconds, 300); assert.equal(r.samplingGaps, 1); assert.equal(r.measurementUnavailableSamples, 5);
  c.complete(1360, 'recovery', true); assert.equal(r.samplingGaps, 1); assert.equal(r.lastSuccessfulSampleAgeSeconds, 0);
  c.update(1600); assert.equal(r.samplingGaps, 2); assert.equal(r.coveragePassed, false);
});

test('fresh run must observe real eight hours and ignores preexisting uptime', async () => {
  const [m] = await modules, r = m.createReport(28800), c = new m.CoverageTracker(r, 80000);
  for (let t = 0; t < 28800; t += 60) c.complete(80000 + t, String(t), true);
  c.update(108799.999); assert.equal(r.durationComplete, false); assert.equal(r.coveragePassed, false);
  c.complete(108800, 'end', true); m.finishReport(r, {completed: true}); assert.equal(r.status, 'PASS');
});

test('trailing stall and never-successful observer cannot pass', async () => {
  const [m] = await modules;
  for (const initial of [false, true]) {
    const r = m.createReport(600), c = new m.CoverageTracker(r, 0);
    if (initial) c.complete(0, 'start', true);
    c.update(600); assert.equal(r.samplingGaps, 1); assert.equal(r.durationComplete, true); assert.equal(r.coveragePassed, false);
  }
});

test('humidity placeholder remains measurement failure while structural data is available', async () => {
  const [m] = await modules, data = row(m.EXPECTED_CARDS);
  const result = m.validateSnapshot(data, {authenticated: true, fresh: true});
  assert.equal(result.complete, true); assert.equal(result.measurementValid, false);
  data.cards.at(-1).value = '0.0'; assert.equal(m.validateSnapshot(data, {authenticated: true, fresh: true}).measurementValid, true);
});

test('cached numeric DOM cannot pass without authentication or fresh data', async () => {
  const [m] = await modules;
  for (const options of [{authenticated: false, fresh: true}, {authenticated: true, fresh: false}])
    assert.equal(m.validateSnapshot(row(m.EXPECTED_CARDS), options).complete, false);
});

test('exact sensor IDs, distinct relay channels and nonblank numeric values are required', async () => {
  const [m] = await modules, options = {authenticated: true, fresh: true};
  for (const invalid of ['', ' ', null, undefined, false, NaN, '--']) {
    const data = row(m.EXPECTED_CARDS); data.cards[0].value = invalid;
    assert.equal(m.validateSnapshot(data, options).complete, false);
  }
  for (const mutate of [r => r.cards[0].id = 'unexpected', r => r.cards[0].id = r.cards[1].id,
    r => r.coils[0].channel = '1', r => r.coils[0].unknown = true,
    r => r.cards.find(c => c.id.includes('soil_temperature')).value = '--']) {
    const data = row(m.EXPECTED_CARDS); mutate(data); assert.equal(m.validateSnapshot(data, options).complete, false);
  }
});

test('401 after login, 429, 503 and page errors are sticky and remain separate from boot 401', async () => {
  const [m] = await modules, r = m.createReport(120), url = 'http://offline.invalid/api/events';
  assert.equal(m.recordHttpStatus(r, 401, url, false), false);
  for (const status of [401, 429, 503]) assert.equal(m.recordHttpStatus(r, status, url, true), true);
  assert.deepEqual(r.httpStatusCounts, {401: 1, 429: 1, 503: 1}); assert.equal(r.http429Count, 1);
  assert.equal(r.authenticationLost, true); m.recordHttpStatus(r, 200, url, true);
  m.finishReport(r); assert.equal(r.stabilityStatus, 'FAIL'); assert.equal(r.status, 'FAIL');
});

test('fatal/interrupt preserves failures and never turns an incomplete measurement into PASS', async () => {
  const [m] = await modules;
  for (const interrupted of [true, false]) {
    const r = m.createReport(28800); r.stabilityStatus = r.measurementStatus = 'FAIL';
    m.finishReport(r, {interrupted}); assert.equal(r.stabilityStatus, 'FAIL'); assert.equal(r.measurementStatus, 'FAIL');
  }
  const r = m.createReport(28800); m.finishReport(r, {interrupted: true});
  assert.equal(r.status, 'INTERRUPTED'); assert.equal(r.measurementStatus, 'INCOMPLETE');
});

test('new error remains visible beyond the first thirty examples', async () => {
  const [m] = await modules, r = m.createReport(28800);
  for (let n = 0; n < 40; n++) m.recordError(r, 'sampling', 'old timeout', String(n));
  const event = m.recordError(r, 'transport', '[WinError 10051] network unreachable', 'latest');
  assert.equal(r.examples.length, 30); assert.equal(r.latestError, event);
  assert.equal(r.errorTypeCounts.NETWORK_UNREACHABLE, 1); assert.equal(r.errorTypeCounts.TIMEOUT, 40);
});

test('read-only guard blocks control, nested writes and repeated login', async () => {
  const [, m] = await modules, base = 'http://offline.invalid';
  for (const route of ['/api/modbus/coil/control', '/api/modbus/motor/control', '/api/periph-exec/run', '/api/system/restart'])
    assert.equal(m.requestAllowed('GET', base + route, '', '', false), false);
  assert.equal(m.requestAllowed('POST', base + '/api/auth/login', '', '', true), true);
  assert.equal(m.requestAllowed('POST', base + '/api/auth/login', '', '', false), false);
  assert.equal(m.requestAllowed('GET', base + '/api/modbus/coil/status', '', '', false), true);
  const batch = requests => m.requestAllowed('POST', base + '/api/batch', JSON.stringify({requests}), 'application/json', false);
  assert.equal(batch([{url: '/api/system/info'}]), true);
  assert.equal(batch([{url: '/api/modbus/coil/control'}]), false);
});

test('numeric humidity placeholder cannot hide a failed measurement', async () => {
  const [m] = await modules, options = {authenticated: true, fresh: true};
  for (const flags of [{valuePlaceholder: 'true'}, {valuePlaceholder: true},
    {measurementValid: 'false'}, {measurementValid: false}, {quality: 'disconnected'}, {quality: 'low'}]) {
    const data = row(m.EXPECTED_CARDS);
    Object.assign(data.cards.at(-1), {value: '0', ...flags});
    const result = m.validateSnapshot(data, options);
    assert.equal(result.complete, true); assert.equal(result.measurementValid, false);
  }
  const data = row(m.EXPECTED_CARDS);
  Object.assign(data.cards.at(-1), {value: '0', measurementValid: 'true', valuePlaceholder: 'false', quality: 'valid'});
  assert.equal(m.validateSnapshot(data, options).measurementValid, true);
});

test('passive SSE observer opens no connections and ignores heartbeat or malformed payloads', async () => {
  const [m] = await modules; let opened = 0, clock = 1000;
  class Source extends EventEmitter {
    constructor() { super(); opened++; }
    addEventListener(type, listener) { this.on(type, listener); }
  }
  const window = {EventSource: Source};
  vm.runInNewContext('(' + m.installSseEvidence.toString() + ')()', {window, performance: {now: () => clock}});
  assert.equal(opened, 0);
  const stream = new window.EventSource('/api/events'); assert.equal(opened, 1);
  stream.emit('heartbeat', {data: 'ping'}); stream.emit('sensor-data', {data: '{'});
  stream.emit('modbus-data', {data: '{}'}); assert.equal(Object.keys(window.__soakDataAt).length, 0);
  stream.emit('modbus-data', {data: '{"channels":[1]}'});
  assert.equal(window.__soakDataAt['modbus-data'], 1000);
  clock = 2000; stream.emit('sensor-data', {data: '{"sensors":[1]}'});
  assert.equal(window.__soakDataAt['sensor-data'], 2000);
  assert.equal(window.__soakDataAt['modbus-data'], 1000);
});

test('passive SSE evidence retains short disconnects and malformed JSON through recovery', async () => {
  const [m] = await modules; let opened = 0, clock = 1000, closed = 0;
  class Source extends EventEmitter {
    constructor() { super(); opened++; }
    addEventListener(type, fn) { this.on(type, fn); }
    close() { closed++; }
  }
  const window = {EventSource: Source};
  const install = () => vm.runInNewContext('(' + m.installSseEvidence.toString() + ')()',
    {window, performance: {now: () => clock}});
  install(); install();
  assert.equal(opened, 0);
  const stream = new window.EventSource('/api/events');
  stream.emit('open'); stream.emit('heartbeat', {data: 'ping'});
  stream.emit('error'); stream.emit('sensor-data', {data: '{bad SECRET'});
  clock = 2000; stream.emit('open'); stream.emit('sensor-data', {data: '{"sensors":[1]}'});
  const info = window.__soakSseConnections[0];
  assert.equal(window.__soakSseConnections.length, 1);
  assert.equal(info.errors, 1); assert.equal(info.invalidJson.length, 1);
  assert.equal(info.events.heartbeat, 1); assert.equal(info.events['sensor-data'], 2);
  assert.equal(window.__soakDataAt['sensor-data'], 2000);
  assert.equal(JSON.stringify(info).includes('SECRET'), false);
  clock = 3000; stream.close();
  assert.equal(info.closedAt, 3000); assert.equal(closed, 1); assert.equal(opened, 1);
});

test('SSE failure counters deduplicate snapshots and never turn PASS after recovery', async () => {
  const [m] = await modules, report = m.createReport(300);
  const failed = [{errors: 2, invalidJson: [{at: 1}]}];
  assert.deepEqual(m.recordSseFailures(report, failed), {streamErrors: 2, jsonErrors: 1});
  assert.deepEqual(m.recordSseFailures(report, failed), {streamErrors: 0, jsonErrors: 0});
  assert.deepEqual(m.recordSseFailures(report, []), {streamErrors: 0, jsonErrors: 0});
  assert.deepEqual(m.recordSseFailures(report, [...failed, {errors: 1, invalidJson: []}]), {streamErrors: 1, jsonErrors: 0});
  Object.assign(report, {durationComplete: true, coveragePassed: true, samples: 6, measurementStatus: 'PASS'});
  m.finishReport(report, {completed: true});
  assert.equal(report.sseStreamErrors, 3); assert.equal(report.sseJsonErrors, 1);
  assert.equal(report.measurementStatus, 'PASS'); assert.equal(report.stabilityStatus, 'FAIL');
  assert.equal(report.status, 'FAIL');
});

async function simulate(mode, seconds = 300) {
  const [evidence, sampler] = await modules;
  const folder = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-soak-offline-')), output = path.join(folder, 'run');
  let clock = 100000, evaluations = 0, peripheralClicks = 0, routeHandler;
  const page = new EventEmitter();
  Object.assign(page, {setDefaultTimeout() {}, async goto() {}, async waitForFunction() {}, async screenshot() {},
    locator(selector) { return {async fill() {}, async waitFor() {}, async click() {
      if (selector.includes('peripheral')) { peripheralClicks++; if (mode === 'navigation') throw Error('locator.click: Timeout 45000ms exceeded.'); }
      if (selector === '#login-button') await routeHandler({request: () => ({method: () => 'POST', url: () => 'http://offline.invalid/api/auth/login', postData: () => '', headers: () => ({})}), async continue() {}, async abort() { throw Error('Initial login unexpectedly blocked'); }});
    }}; },
    async evaluate() {
      evaluations++;
      const data = row(evidence.EXPECTED_CARDS);
      if (mode === 'sse-recovered') {
        data.cards.at(-1).value = '5.8';
        data.sseConnections = [{errors: 1, invalidJson: [{at: 1, event: 'sensor-data'}]}];
      }
      if (mode === 'missing-modbus') data.cards.find(card => card.id === 'pm25').value = '--';
      if (evaluations > 1) {
        if (mode === 'closed') throw Error('page.evaluate: Target page, context or browser has been closed');
        if (mode === 'failure') throw Error('locator.click: Timeout 45000ms exceeded.');
        if (mode === '401') page.emit('response', {url: () => 'http://offline.invalid/api/events', status: () => 401});
        if (mode === 'stale') data.sseAges = {'modbus-data': 0, 'sensor-data': 181};
      }
      return data;
    }});
  const identity = {status: () => 200, ok: () => true, json: async () => ({success: true, data: {network: {macAddress: '14:C1:9F:C1:52:D8'}}})};
  const context = {addInitScript: async () => {}, newPage: async () => page, route: async (glob, handler) => {routeHandler = handler;}, request: {get: async () => identity}};
  const chromium = {launch: async () => ({newContext: async () => context, close: async () => {}})};
  try {
    const report = await sampler.runBrowserSoak({url: 'http://offline.invalid', 'expected-mac': '14:C1:9F:C1:52:D8', seconds, out: output},
      {chromium, now: () => clock, sleep: async delay => {
        clock += delay;
        if (mode === 'closed-idle') page.emit('close');
        if (mode === 'crashed-idle') page.emit('crash');
      }});
    const rows = fs.readFileSync(path.join(output, 'samples.jsonl'), 'utf8').trim().split('\n').map(JSON.parse);
    const eventsPath = path.join(output, 'events.jsonl');
    const events = fs.existsSync(eventsPath) ? fs.readFileSync(eventsPath, 'utf8').trim().split('\n').map(JSON.parse) : [];
    assert.deepEqual(JSON.parse(fs.readFileSync(path.join(output, 'report.json'), 'utf8')), report);
    return {report, rows, events, peripheralClicks};
  } finally { fs.rmSync(folder, {recursive: true, force: true}); }
}

test('runner persists all failed attempts and identifies successful-sample outage', async () => {
  const {report, rows, events} = await simulate('failure');
  assert.equal(rows.length, report.attempts); assert.equal(rows.length, 6); assert.equal(report.samples, 0);
  assert.ok(rows.every(r => r.complete === false && r.error)); assert.equal(report.samplingGaps, 1);
  assert.equal(report.stabilityStatus, 'FAIL'); assert.equal(report.measurementStatus, 'FAIL');
  assert.equal(report.durationComplete, true); assert.equal(report.coveragePassed, false); assert.equal(events.length, 6);
});

test('runner rejects numeric cached DOM after 401 and does not re-login', async () => {
  const {report, rows} = await simulate('401');
  assert.equal(report.httpStatusCounts['401'], 1); assert.equal(report.authenticationLost, true);
  assert.equal(report.samples, 0); assert.equal(report.blockedWrites, 0);
  assert.ok(rows.every(r => !r.complete)); assert.match(rows[1].error, /Authentication lost/);
});

test('runner rejects one stale measurement stream despite fresh Modbus traffic', async () => {
  const {report, rows} = await simulate('stale');
  assert.equal(report.samples, 0); assert.ok(rows.every(r => r.fresh === false)); assert.equal(report.stabilityStatus, 'FAIL');
});

test('runner separates stable page from known invalid humidity', async () => {
  const {report} = await simulate('healthy');
  assert.equal(report.stabilityStatus, 'PASS'); assert.equal(report.coveragePassed, true);
  assert.equal(report.measurementStatus, 'FAIL'); assert.equal(report.status, 'FAIL');
});

test('runner persists recovered SSE failures once while retaining all valid samples', async () => {
  const {report, rows, events} = await simulate('sse-recovered');
  assert.equal(rows.length, 6); assert.equal(report.samples, 6);
  assert.ok(rows.every(row => row.complete && row.sseConnections[0].errors === 1));
  assert.equal(report.sseStreamErrors, 1); assert.equal(report.sseJsonErrors, 1);
  assert.equal(events.filter(event => event.kind === 'sseStream').length, 1);
  assert.equal(events.filter(event => event.kind === 'sseJson').length, 1);
  assert.equal(report.coveragePassed, true); assert.equal(report.measurementStatus, 'PASS');
  assert.equal(report.status, 'FAIL');
});

test('unavailable Modbus samples remain failures for the full requested observation', async () => {
  const {report, rows} = await simulate('missing-modbus');
  assert.equal(rows.length, 6); assert.equal(report.attempts, 6);
  assert.equal(report.samples, 0); assert.equal(report.durationComplete, true);
  assert.equal(report.coveragePassed, false); assert.equal(report.status, 'FAIL');
  assert.ok(rows.every(row => !row.complete && row.modbusAvailable === false));
  assert.equal(report.measurementStatus, 'FAIL'); assert.equal(report.stabilityStatus, 'FAIL');
});

test('failed periodic navigation does not trigger forty-five-second retries every sample', async () => {
  const {report, peripheralClicks} = await simulate('navigation', 720);
  assert.equal(peripheralClicks, 1); assert.equal(report.displayErrors, 1); assert.equal(report.stabilityStatus, 'FAIL');
});

test('permanently closed browser ends incomplete and keeps the failed attempt', async () => {
  const {report, rows, events} = await simulate('closed', 28800);
  assert.equal(rows.length, 1); assert.equal(rows[0].complete, false);
  assert.equal(report.status, 'FAIL'); assert.equal(report.durationComplete, false);
  assert.equal(report.coveragePassed, false); assert.equal(report.completedAt, undefined);
  assert.ok(report.terminatedAt); assert.equal(report.terminationReason, 'BROWSER_UNAVAILABLE');
  assert.equal(events.filter(event => event.kind === 'observerClosed').length, 1);
});

test('page closure or crash while waiting stops without extending the observation', async () => {
  for (const mode of ['closed-idle', 'crashed-idle']) {
    const {report, rows} = await simulate(mode, 28800);
    assert.equal(rows.length, 1); assert.equal(report.attempts, 1);
    assert.equal(report.status, 'FAIL'); assert.equal(report.durationComplete, false);
    assert.equal(report.coveragePassed, false); assert.ok(report.observerLostAt);
    assert.ok(report.terminatedAt); assert.equal(report.completedAt, undefined);
  }
});
