/** Read-only panel soak. RUNNING and interrupted runs are never acceptance passes. */
import fs from 'node:fs/promises';
import path from 'node:path';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { isIP } from 'node:net';
import { pathToFileURL } from 'node:url';

const arg = (key, fallback) => process.argv.find(value => value.startsWith('--' + key + '='))?.slice(key.length + 3) ?? fallback;
export const normalizeMac = value => String(value || '').replace(/[^0-9a-f]/gi, '').toLowerCase();
export const allowedGet = route => {
  if (typeof route !== 'string' || !route.startsWith('/api/') || route.includes('\\')) return false;
  try {
    const url = new URL(route, 'http://device');
    return url.origin === 'http://device' && ['/api/network/status', '/api/health', '/api/system/info', '/api/screen-panel/config', '/api/screen-panel/state', '/api/screen-panel/weather/refresh', '/api/periph-exec/history'].includes(url.pathname);
  } catch { return false; }
};
export const hasFreshIdentity = (neighbors, ip, mac) => Array.isArray(neighbors) && neighbors.some(row =>
  row.IPAddress === ip && row.State === 'Reachable' && normalizeMac(row.LinkLayerAddress) === normalizeMac(mac));
export async function readReachableNeighbors(ip) {
  assert.equal(isIP(ip), 4, 'A literal IPv4 target is required');
  assert.equal(process.platform, 'win32', 'Fresh neighbor verification requires the Windows observation host');
  try {
    const { stdout } = await promisify(execFile)('powershell.exe', ['-NoProfile', '-Command',
      "Get-NetNeighbor -IPAddress '" + ip + "' -ErrorAction Stop | Select-Object IPAddress,LinkLayerAddress,@{n='State';e={$_.State.ToString()}} | ConvertTo-Json -Compress"],
    { encoding: 'utf8', timeout: 30000, windowsHide: true });
    return [].concat(JSON.parse(stdout));
  } catch (error) {
    throw new Error('Local neighbor lookup failed: ' + (error.killed ? 'process timeout (30000ms)' : error.code || error.name), {cause: error});
  }
}
function contractOnly() {
  assert(allowedGet('/api/periph-exec/history?sourceRef=local.temp'));
  assert(!allowedGet('/api/modbus/motor/control'));
  assert(!allowedGet('/api/periph-exec/run'));
  assert(!allowedGet('http://other/api/health'));
  assert.equal(normalizeMac('E8:3D:C1:F7:65:44'), 'e83dc1f76544');
  console.log('PASS: panel soak reads only whitelisted routes; no actuation or configuration writes');
}
export async function runScreenPanelSoak(options, dependencies = {}) {
  const target = new URL(options.url);
  assert(['http:', 'https:'].includes(target.protocol) && !target.username && !target.password &&
    target.pathname === '/' && !target.search && !target.hash, 'Provide a credential-free device origin');
  assert.equal(isIP(target.hostname), 4, 'A literal IPv4 target is required');
  const origin = target.origin;
  assert(/^(?:[\da-f]{12}|(?:[\da-f]{2}:){5}[\da-f]{2}|(?:[\da-f]{2}-){5}[\da-f]{2})$/i.test(options.expectedMac), 'Invalid expected MAC');
  const expectedMac = normalizeMac(options.expectedMac);
  assert.equal(expectedMac.length, 12, 'Provide --expected-mac');
  const seconds = Number(options.seconds ?? 86400), interval = Number(options.interval ?? 60);
  assert(Number.isInteger(seconds) && seconds >= 30 && seconds <= 172800);
  assert(Number.isInteger(interval) && interval >= 10 && interval <= 300);
  const out = path.resolve(options.out);
  const fetch = dependencies.fetch || globalThis.fetch;
  const monotonic = dependencies.monotonic || (() => performance.now());
  const now = dependencies.now || Date.now;
  const timestamp = () => new Date(now()).toISOString();
  const sleep = dependencies.sleep || (milliseconds => new Promise(resolve => setTimeout(resolve, milliseconds)));
  const readNeighbors = dependencies.readNeighbors || readReachableNeighbors;
  const stopping = () => options.signal?.aborted === true;
  const deploymentPath = path.resolve(options.deployment);
  const deploymentBytes = await fs.readFile(deploymentPath);
  const sha = value => createHash('sha256').update(value).digest('hex');
  const deploymentDigest = sha(deploymentBytes);
  const deployment = JSON.parse(deploymentBytes);
  assert(deployment.written === true && deployment.verified === true, 'Verified deployment required');
  let plan;
  try { plan = JSON.parse(await fs.readFile(options.plan || path.join(path.dirname(deploymentPath), 'soak-observation-plan.json'), 'utf8')); }
  catch (error) { if (error.code !== 'ENOENT' || options.plan) throw error; }
  await fs.mkdir(out, { recursive: false });
  let token = '', trusted = false, stage = 'preflight', previousUptime, previousTick, weatherStamp, firstHeap, lastHeap;
  let sampleStart, deadline, baselineRevision, baselineConfigHash;
  const username = options.credentials?.username || process.env.FASTBEE_USERNAME || 'admin';
  const password = options.credentials?.password || process.env.FASTBEE_PASSWORD || 'admin123';
  const secrets = new Set([username, password]);
  const redact = value => {
    let text = String(value);
    // Replace longer secrets first when a username is a prefix of a password.
    for (const secret of [...secrets].filter(Boolean).sort((a, b) => b.length - a.length)) text = text.split(secret).join('[redacted]');
    return text.slice(0, 160);
  };
  const initialHeaps = [], recentHeaps = [];
  const median = values => values.slice().sort((a, b) => a - b)[Math.floor(values.length / 2)];
  const start = monotonic();
  const report = { status: 'RUNNING', stabilityStatus: 'RUNNING', acceptanceStatus: 'INCOMPLETE', pid: process.pid,
    startedAt: timestamp(), plannedEndAt: null, continuous24hStatus: 'NOT_RUN',
    requestedSeconds: seconds, intervalSeconds: interval, firmwareSha256: deployment.firmwareSha256,
    deploymentReportSha256: deploymentDigest, preflightFailures: 0, initializationFailures: 0,
    identityFailures: 0, touchFailures: 0, configChanges: 0, deploymentChanges: 0,
    webSha256: deployment.webSha256, expectedMac, samples: 0, transportErrors: 0, http429Count: 0,
    restarts: 0, samplingGaps: 0, runtimeFailures: 0, degradedSamples: 0, staleWeatherSamples: 0,
    weatherUpdates: 0, historyUpdates: {}, missingHistorySamples: 0, staleHistorySamples: 0, dataStatus: 'INCOMPLETE', errors: [], requestFailures: [],
    unverified: ['physical-touch', 'physical-swipe', 'physical-brightness', 'physical-buzzer', 'physical-motor-response', 'physical-relay-load', 'optical-display-integrity'] };
  const save = async () => {
    report.updatedAt = timestamp(); report.elapsedSeconds = Math.round((monotonic() - start) / 1000);
    const temp = path.join(out, 'report.pending');
    await fs.writeFile(temp, JSON.stringify(report, null, 2)); await fs.rename(temp, path.join(out, 'report.json'));
  };
  const note = (kind, message) => { if (report.errors.length < 30) report.errors.push({ at: timestamp(), kind, message: redact(message) }); };
  const fatal = (counter, message) => { ++report[counter]; const error = new Error(message); error.fatal = true; throw error; };
  async function exchange(route, options = {}) {
    const response = await fetch(origin + route, { redirect: 'error', signal: AbortSignal.timeout(15000), ...options });
    if (response.status === 429) ++report.http429Count;
    return response;
  }
  async function verifyIdentityBeforeLogin() {
    trusted = false;
    const response = await exchange('/api/health');
    await response.arrayBuffer();
    if (!response.ok) throw new Error('Identity preflight health: HTTP ' + response.status);
    report.neighbor = await readNeighbors(target.hostname);
    if (!hasFreshIdentity(report.neighbor, target.hostname, expectedMac)) fatal('identityFailures', 'Fresh expected neighbor MAC required before login');
    trusted = true;
  }
  async function checkDeployment() {
    if (sha(await fs.readFile(deploymentPath)) !== deploymentDigest) fatal('deploymentChanges', 'Deployment report changed during observation');
  }
  function inspectConfig(config) {
    assert(Number.isInteger(config.revision) && Array.isArray(config.config?.pages), 'Missing panel revision or pages');
    const digest = sha(JSON.stringify(config.config));
    baselineRevision ??= plan?.expectedRevision ?? config.revision;
    baselineConfigHash ??= plan?.expectedConfigSha256 ?? digest;
    if (config.revision !== baselineRevision || digest !== baselineConfigHash) fatal('configChanges', 'Panel configuration changed');
    report.revision = config.revision; report.configSha256 = digest;
  }
  await save();
  async function login() {
    assert(trusted, 'Identity verification required before authentication');
    const response = await exchange('/api/auth/login', { method: 'POST',
      headers: { 'X-FastBee-Api-Client': 'panel-soak' },
      body: new URLSearchParams({ username, password }) });
    const body = await response.json();
    if (!response.ok || body.success !== true) throw new Error('Authentication failed');
    token = body.sessionId || body.data?.sessionId;
    if (!token) throw new Error('Missing session');
    secrets.add(token);
  }
  async function get(route) {
    assert(allowedGet(route));
    let phase = 'headers', response, requestStarted, headersReceived;
    try {
      for (let attempt = 0; attempt < 2; attempt++) {
        phase = 'headers'; response = undefined; requestStarted = monotonic(); headersReceived = undefined;
        response = await exchange(route, {
          headers: { Authorization: 'Bearer ' + token, 'X-FastBee-Api-Client': 'panel-soak' } });
        headersReceived = monotonic(); phase = 'body';
        if (response.status === 401 && !attempt) {
          await response.arrayBuffer(); token = '';
          phase = 'session-renewal';
          await verifyIdentityBeforeLogin(); await login(); continue;
        }
        if (!response.ok) { await response.arrayBuffer(); throw new Error(route.split('?')[0] + ': HTTP ' + response.status); }
        const body = await response.json();
        if (body.success !== true) throw new Error('API rejected ' + route.split('?')[0]);
        await sleep(350);
        return body.data || body;
      }
      throw new Error('Session renewal failed');
    } catch (error) {
      const endpoint = route.split('?')[0];
      if (report.requestFailures.length < 30) report.requestFailures.push({
        at: timestamp(), route: endpoint, phase, status: response?.status,
        elapsedMs: Math.round(monotonic() - requestStarted),
        headersMs: headersReceived === undefined ? undefined : Math.round(headersReceived - requestStarted),
        contentLength: response?.headers?.get('content-length'),
        causeCode: redact(error.cause?.code || error.code || ''), message: redact(error.message),
      });
      error.message = endpoint + ' [' + phase + ']: ' + error.message;
      throw error;
    }
  }
  try {
    if (stopping()) { report.status = 'INCOMPLETE'; report.stabilityStatus = 'INCOMPLETE'; return report; }
    if (plan && (plan.deploymentReportSha256 !== deploymentDigest || plan.webSha256 !== deployment.webSha256 ||
      plan.firmwareSha256 !== deployment.firmwareSha256 || normalizeMac(plan.expectedMac) !== expectedMac || plan.url !== origin)) {
      fatal('deploymentChanges', 'Observation plan does not match target deployment');
    }
    await verifyIdentityBeforeLogin();
    stage = 'initialization';
    await login();
    const identity = await get('/api/network/status');
    if (normalizeMac(identity.macAddress) !== expectedMac) {
      trusted = false; token = ''; fatal('identityFailures', 'Wrong device');
    }
    inspectConfig(await get('/api/screen-panel/config'));
    stage = 'sampling'; sampleStart = monotonic(); deadline = sampleStart + seconds * 1000;
    report.samplingStartedAt = timestamp(); report.plannedEndAt = new Date(now() + seconds * 1000).toISOString();
    let historySources = [], historyStamps = {}, weatherId = '';
    do {
      if (stopping()) break;
      const tick = monotonic(), row = { at: timestamp() };
      if (previousTick !== undefined && tick - previousTick > interval * 3000) ++report.samplingGaps;
      previousTick = tick;
      try {
        await checkDeployment();
        const info = await get('/api/system/info');
        if (normalizeMac(info.network?.macAddress) !== expectedMac) {
          trusted = false; token = ''; fatal('identityFailures', 'Device identity changed');
        }
        const health = await get('/api/health');
        const uptime = info.uptime?.seconds;
        assert(Number.isFinite(uptime), 'Missing uptime');
        if (previousUptime !== undefined && uptime < previousUptime) ++report.restarts;
        previousUptime = uptime; report.initialUptime ??= uptime; report.finalUptime = uptime;
        const heap = health.memory?.heapFree, largest = health.memory?.heapMaxAlloc;
        assert(Number.isFinite(heap) && heap >= 0 && Number.isFinite(largest) && largest >= 0, 'Missing or invalid heap metrics');
        if (Number.isFinite(heap)) {
          firstHeap ??= heap; lastHeap = heap; report.minHeap = Math.min(report.minHeap ?? heap, heap);
          if (report.samples >= 10 && initialHeaps.length < 10) initialHeaps.push(heap);
          recentHeaps.push(heap); if (recentHeaps.length > 10) recentHeaps.shift();
        }
        if (Number.isFinite(largest)) report.minLargestBlock = Math.min(report.minLargestBlock ?? largest, largest);
        if (health.degraded || health.memory?.critical || health.memory?.degraded) ++report.degradedSamples;
        const state = await get('/api/screen-panel/state');
        row.uptime = uptime; row.memory = health.memory; row.runtime = state.application?.runtime;
        row.touchReady = state.capabilities?.touchReady === true; row.touchError = redact(state.capabilities?.touchError || '');
        row.revision = state.revision;
        if (!row.touchReady || row.touchError) ++report.touchFailures;
        if (state.revision !== baselineRevision) fatal('configChanges', 'Panel revision changed');
        if (row.runtime?.running !== true) ++report.runtimeFailures;
        if (report.samples % 5 === 0) {
          const config = await get('/api/screen-panel/config');
          inspectConfig(config);
          const cards = config.config.pages.flatMap(page => page.cards || []);
          weatherId = cards.find(card => card.kind === 'weather')?.sourceRef?.replace(/^weather\./, '') || '';
          historySources = [...new Set(cards.filter(card => card.kind === 'chart' && card.visualType === 'line').map(card => card.sourceRef).filter(Boolean))];
          if (weatherId) {
            const weather = await get('/api/screen-panel/weather/refresh?id=' + encodeURIComponent(weatherId));
            row.weather = { status: weather.status, reason: weather.reason, updatedMs: weather.updatedMs, stale: weather.stale, ageMs: weather.ageMs, memory: weather.memory };
            if (weatherStamp !== undefined && weather.updatedMs !== weatherStamp) ++report.weatherUpdates;
            weatherStamp = weather.updatedMs;
            if (weather.stale || !weather.updatedMs) ++report.staleWeatherSamples;
            report.lastWeather = row.weather;
          }
          row.histories = {};
          for (const ref of historySources) {
            const series = await get('/api/periph-exec/history?sourceRef=' + encodeURIComponent(ref));
            const last = series.points?.at(-1)?.seq;
            row.histories[ref] = { count: series.points?.length || 0, lastSequence: last, stale: series.stale };
            report.historyUpdates[ref] ??= 0;
            if (last !== undefined && historyStamps[ref] !== undefined && last !== historyStamps[ref]) ++report.historyUpdates[ref];
            historyStamps[ref] = last;
            if (!series.points?.length) ++report.missingHistorySamples;
            if (series.stale) ++report.staleHistorySamples;
          }
        }
        ++report.samples;
        report.firstActualSampleAt ??= row.at; report.lastActualSampleAt = row.at;
      } catch (error) {
        row.error = redact(error.message);
        if (error.fatal) { await fs.appendFile(path.join(out, 'samples.jsonl'), JSON.stringify(row) + '\n'); throw error; }
        ++report.transportErrors; note('sample', error.message);
      }
      await fs.appendFile(path.join(out, 'samples.jsonl'), JSON.stringify(row) + '\n');
      await save();
      if (monotonic() >= deadline || stopping()) break;
      await sleep(Math.max(0, Math.min(Math.max(0, interval * 1000 - (monotonic() - tick)), deadline - monotonic())));
    } while (!stopping());
    await checkDeployment();
    if (!stopping()) inspectConfig(await get('/api/screen-panel/config'));
    report.heapChangeBytes = lastHeap - firstHeap;
    report.heapTrend = { baselineMedian: median(initialHeaps), finalMedian: median(recentHeaps) };
    report.sustainedHeapDecline = report.samples >= 60 && report.heapTrend.baselineMedian - report.heapTrend.finalMedian > 8192 && report.heapTrend.finalMedian < report.heapTrend.baselineMedian * .75;
    report.actualSampleSpanSeconds = report.samples > 1 ? (Date.parse(report.lastActualSampleAt) - Date.parse(report.firstActualSampleAt)) / 1000 : 0;
    const covered = !stopping() && monotonic() >= deadline && report.samples >= Math.max(2, Math.ceil(seconds / interval * .9)) && report.actualSampleSpanSeconds >= seconds * .9;
    const stable = covered && !['transportErrors', 'http429Count', 'restarts', 'samplingGaps', 'runtimeFailures', 'touchFailures', 'configChanges', 'deploymentChanges', 'degradedSamples', 'sustainedHeapDecline'].some(key => report[key]);
    report.stabilityStatus = stable ? 'PASS' : covered ? 'FAIL' : 'INCOMPLETE';
    report.dataStatus = !covered ? 'INCOMPLETE' : report.staleWeatherSamples || report.missingHistorySamples || report.staleHistorySamples ||
      (seconds >= 7200 && (weatherId && !report.weatherUpdates || historySources.some(ref => !report.historyUpdates[ref]))) ? 'FAIL' : 'PASS';
    report.status = stable && report.dataStatus === 'PASS' ? 'PASS' : covered ? 'FAIL' : 'INCOMPLETE';
  } catch (error) {
    if (stage === 'preflight') ++report.preflightFailures;
    else if (stage === 'initialization') ++report.initializationFailures;
    report.status = 'FAIL'; report.stabilityStatus = 'INCOMPLETE'; note('fatal', error.message);
  } finally {
    if (token && trusted) await exchange('/api/auth/logout', { method: 'POST', headers: { Authorization: 'Bearer ' + token } }).then(async response => {
      await response.arrayBuffer(); if (!response.ok) throw new Error('Logout: HTTP ' + response.status);
    }).catch(error => { ++report.transportErrors; note('cleanup', error.message); report.status = 'FAIL'; report.stabilityStatus = 'FAIL'; });
    token = '';
    report.actualSampleSpanSeconds = report.samples > 1 ? (Date.parse(report.lastActualSampleAt) - Date.parse(report.firstActualSampleAt)) / 1000 : 0;
    report.completedAt = timestamp(); await save();
  }
  return report;
}

if (process.argv[1] && pathToFileURL(path.resolve(process.argv[1])).href === import.meta.url) {
  if (process.argv.includes('--contract-only')) contractOnly();
  else {
    const controller = new AbortController();
    const stop = () => controller.abort();
    process.on('SIGINT', stop); process.on('SIGTERM', stop);
    try {
      const report = await runScreenPanelSoak({ url: arg('url'), expectedMac: arg('expected-mac'), seconds: arg('seconds'),
        interval: arg('interval'), out: arg('out'), deployment: arg('deployment'), plan: arg('plan'), signal: controller.signal });
      console.log(JSON.stringify(report)); process.exitCode = report.status === 'PASS' ? 0 : 2;
    } catch (error) { console.error(error.message); process.exitCode = 2; }
    finally { process.removeListener('SIGINT', stop); process.removeListener('SIGTERM', stop); }
  }
}
