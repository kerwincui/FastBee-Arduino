import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { allowedGet, hasFreshIdentity, runScreenPanelSoak } from '../../scripts/test-screen-panel-soak.mjs';

// Synthetic HTTP responses and a virtual clock exercise the production runner.
// No network, serial port, real device credentials, or wall-clock waits are used.
const URL = 'http://127.0.0.2:9';
const MAC = '02:00:00:00:00:01';
const OTHER_MAC = '02:00:00:00:00:02';
const EPOCH = Date.parse('2026-09-28T00:00:00.000Z');
const digest = value => createHash('sha256').update(value).digest('hex');
const ok = data => Response.json({ success: true, data });
const httpError = status => Response.json({ success: false }, { status });

async function fixture(t, hook) {
  const root = await fs.mkdtemp(path.join(os.tmpdir(), 'fastbee-panel-soak-test-'));
  t.after(() => fs.rm(root, { recursive: true, force: true }));
  const deployment = { written: true, verified: true, firmwareSha256: 'a'.repeat(64), webSha256: 'b'.repeat(64) };
  const deploymentPath = path.join(root, 'deployment.json');
  await fs.writeFile(deploymentPath, JSON.stringify(deployment));
  const f = {
    root, deployment, deploymentPath, clock: 0, wallOffset: 0, calls: [], visits: {}, active: 0, maxActive: 0,
    neighborReads: 0, neighbors: [{ IPAddress: '127.0.0.2', State: 'Reachable', LinkLayerAddress: MAC }],
    config: { schemaVersion: 1, pages: [{ id: 'home', cards: [
      { kind: 'weather', sourceRef: 'weather.test' },
      { kind: 'chart', visualType: 'line', sourceRef: 'test.temperature' },
    ] }] },
    credentials: { username: 'offline-user', password: 'offline-password-value' },
    controller: new AbortController(),
  };
  f.options = { url: URL, expectedMac: MAC, seconds: 60, interval: 15,
    out: path.join(root, 'run'), deployment: deploymentPath, credentials: f.credentials, signal: f.controller.signal };
  f.plan = async (overrides = {}) => {
    await fs.writeFile(path.join(root, 'soak-observation-plan.json'), JSON.stringify({
      url: URL, expectedMac: MAC, ...deployment,
      deploymentReportSha256: digest(await fs.readFile(deploymentPath)),
      expectedRevision: 28, expectedConfigSha256: digest(JSON.stringify(f.config)), ...overrides,
    }));
  };
  f.defaults = pathname => {
    switch (pathname) {
      case '/api/auth/login': return Response.json({ success: true, sessionId: 'offline-token-' + f.visits[pathname] });
      case '/api/auth/logout': return ok({});
      case '/api/network/status': return ok({ macAddress: MAC });
      case '/api/health': return ok({ memory: { heapFree: 60000, heapMaxAlloc: 30000 } });
      case '/api/system/info': return ok({ network: { macAddress: MAC }, uptime: { seconds: 100 + Math.floor(f.clock / 1000) } });
      case '/api/screen-panel/config': return ok({ revision: 28, config: structuredClone(f.config) });
      case '/api/screen-panel/state': return ok({ revision: 28, capabilities: { touchReady: true, touchError: '' }, application: { runtime: { running: true } } });
      case '/api/screen-panel/weather/refresh': return ok({ status: 'ready', updatedMs: 1000, stale: false, ageMs: f.clock });
      case '/api/periph-exec/history': return ok({ points: [{ seq: Math.floor(f.clock / 1000) + 1, value: 20 }], stale: false });
      default: assert.fail('Unexpected offline route: ' + pathname);
    }
  };
  f.dependencies = {
    monotonic: () => f.clock,
    now: () => EPOCH + f.clock + f.wallOffset,
    sleep: async ms => { assert(ms >= 0); f.clock += ms; },
    readNeighbors: async ip => { assert.equal(ip, '127.0.0.2'); f.neighborReads++; return f.neighbors; },
    fetch: async (url, options) => {
      assert(url.startsWith(URL + '/api/'), 'Tests must never reach a real host');
      const route = new globalThis.URL(url);
      const call = { pathname: route.pathname, search: route.search, method: options.method || 'GET',
        authorization: options.headers?.Authorization, body: options.body, at: f.clock };
      f.calls.push(call);
      f.visits[call.pathname] = (f.visits[call.pathname] || 0) + 1;
      assert.equal(options.redirect, 'error');
      assert(options.signal instanceof AbortSignal);
      if (call.method === 'POST') assert(['/api/auth/login', '/api/auth/logout'].includes(call.pathname));
      else { assert.equal(call.method, 'GET'); assert(allowedGet(route.pathname + route.search)); }
      f.maxActive = Math.max(f.maxActive, ++f.active);
      try { return await hook?.(call, f) ?? f.defaults(call.pathname); }
      finally { f.active--; }
    },
  };
  f.run = async (overrides = {}) => {
    const report = await runScreenPanelSoak({ ...f.options, ...overrides }, f.dependencies);
    assert.equal(f.maxActive, f.calls.length ? 1 : 0, 'HTTP requests must stay serial');
    const saved = JSON.parse(await fs.readFile(path.join(f.options.out, 'report.json'), 'utf8'));
    assert.equal(saved.status, report.status);
    assert.equal(saved.samples, report.samples);
    assert.equal(saved.continuous24hStatus, 'NOT_RUN');
    assert.equal(saved.acceptanceStatus, 'INCOMPLETE');
    return report;
  };
  f.posts = () => f.calls.filter(call => call.method === 'POST');
  return f;
}

test('finite healthy window records actual coverage, no actuation and no physical/24h claim', async t => {
  const f = await fixture(t);
  await f.plan();
  const report = await f.run();
  assert.equal(report.status, 'PASS');
  assert.equal(report.samples, 5);
  assert.equal(report.actualSampleSpanSeconds, 60);
  assert.equal(report.weatherUpdates, 0, 'A 60s window need not update 1800s weather');
  assert.equal(report.revision, 28);
  assert.equal(report.configSha256, digest(JSON.stringify(f.config)));
  assert.equal(report.deploymentReportSha256, digest(await fs.readFile(f.deploymentPath)));
  assert(report.finalUptime > report.initialUptime);
  assert(report.unverified.includes('physical-swipe'));
  assert(report.unverified.includes('physical-motor-response'));
  assert.deepEqual(f.posts().map(call => call.pathname), ['/api/auth/login', '/api/auth/logout']);
  assert.equal(f.calls[0].authorization, undefined);
  assert.equal(f.calls[0].pathname, '/api/health');
  const samples = (await fs.readFile(path.join(f.options.out, 'samples.jsonl'), 'utf8')).trim().split('\n').map(JSON.parse);
  assert.equal(samples.length, report.samples);
  assert(samples.every(row => row.touchReady && !row.touchError));
});

test('route allowlist and neighbor gate reject unsafe targets', () => {
  for (const route of ['/api/modbus/motor/control', '/api/periph-exec/run', '/api/system/restart',
    '//elsewhere/api/health', 'http://elsewhere/api/health', '/api/health/extra', '/api/health\\x', '/api/../health']) {
    assert.equal(allowedGet(route), false, route);
  }
  assert.equal(allowedGet('/api/periph-exec/history?sourceRef=test.temperature'), true);
  assert.equal(hasFreshIdentity([{ IPAddress: '127.0.0.2', State: 'Reachable', LinkLayerAddress: MAC.replaceAll(':', '-') }], '127.0.0.2', MAC), true);
});

test('invalid URL, MAC or timing fails before any request', async t => {
  const f = await fixture(t);
  for (const options of [{ url: 'http://name.local' }, { url: 'http://user:secret@127.0.0.2' },
    { url: URL + '/api/health' }, { url: URL + '?secret=x' }, { url: 'ftp://127.0.0.2' },
    { expectedMac: '02:00:00:00:00:01-not-a-mac' }, { seconds: 0 }, { interval: 1 }]) {
    await assert.rejects(f.run(options));
  }
  assert.equal(f.calls.length, 0);
});

test('connection refusal is a preflight failure, not a zero-error successful run', async t => {
  const f = await fixture(t, () => { throw new Error('connect ECONNREFUSED'); });
  const report = await f.run();
  assert.equal(report.status, 'FAIL');
  assert.equal(report.preflightFailures, 1);
  assert.equal(report.initializationFailures, 0);
  assert.equal(report.samples, 0);
  assert.equal(f.posts().length, 0);
});

for (const [name, row] of [
  ['wrong MAC', { LinkLayerAddress: OTHER_MAC }], ['stale neighbor', { State: 'Stale' }],
  ['probe neighbor', { State: 'Probe' }], ['wrong IP', { IPAddress: '127.0.0.3' }], ['absent neighbor', null],
]) test(name + ' blocks all credentials', async t => {
  const f = await fixture(t);
  f.neighbors = row ? [{ ...f.neighbors[0], ...row }] : [];
  const report = await f.run();
  assert.equal(report.status, 'FAIL');
  assert.equal(report.identityFailures, 1);
  assert.equal(report.samples, 0);
  assert.equal(f.posts().length, 0);
  assert(f.calls.every(call => !call.authorization));
});

test('authentication failure is counted separately from sampling errors', async t => {
  const f = await fixture(t, call => call.pathname === '/api/auth/login' ? httpError(403) : undefined);
  const report = await f.run();
  assert.equal(report.initializationFailures, 1);
  assert.equal(report.transportErrors, 0);
  assert.equal(report.samples, 0);
  assert.equal(report.status, 'FAIL');
  assert.equal(f.posts().length, 1);
});

test('authenticated identity mismatch stops without sampling or sending logout token', async t => {
  const f = await fixture(t, call => call.pathname === '/api/network/status' ? ok({ macAddress: OTHER_MAC }) : undefined);
  const report = await f.run();
  assert.equal(report.identityFailures, 1);
  assert.equal(report.initializationFailures, 1);
  assert.equal(report.samples, 0);
  assert.deepEqual(f.posts().map(call => call.pathname), ['/api/auth/login']);
});

test('401 renews once and rechecks fresh identity before login', async t => {
  const f = await fixture(t, (call, f) => call.pathname === '/api/system/info' && f.visits[call.pathname] === 1 ? httpError(401) : undefined);
  assert.equal((await f.run()).status, 'PASS');
  assert.equal(f.neighborReads, 2);
  const index = f.calls.findIndex(call => call.pathname === '/api/system/info');
  assert.deepEqual(f.calls.slice(index, index + 4).map(call => call.pathname), [
    '/api/system/info', '/api/health', '/api/auth/login', '/api/system/info',
  ]);
  assert.equal(f.calls[index + 1].authorization, undefined);
  assert.equal(f.calls[index + 3].authorization, 'Bearer offline-token-2');
});

test('401 with changed neighbor never forwards credentials or cleanup token', async t => {
  const f = await fixture(t, (call, f) => {
    if (call.pathname === '/api/system/info') {
      f.neighbors = [{ ...f.neighbors[0], LinkLayerAddress: OTHER_MAC }];
      return httpError(401);
    }
  });
  const report = await f.run();
  assert.equal(report.status, 'FAIL');
  assert.equal(report.identityFailures, 1);
  assert.equal(report.samples, 0);
  assert.deepEqual(f.posts().map(call => call.pathname), ['/api/auth/login']);
});

for (const capabilities of [{ touchReady: false }, {}, { touchReady: true, touchError: 'controller unavailable' }]) {
  test('touch failure remains visible despite healthy HTTP and running runtime: ' + JSON.stringify(capabilities), async t => {
    const f = await fixture(t, call => call.pathname === '/api/screen-panel/state' ? ok({ revision: 28, capabilities, application: { runtime: { running: true } } }) : undefined);
    const report = await f.run();
    assert.equal(report.status, 'FAIL');
    assert.equal(report.touchFailures, report.samples);
    assert.equal(report.transportErrors, 0);
    assert.equal(report.runtimeFailures, 0);
  });
}

test('runtime stoppage and memory degradation have independent counters', async t => {
  const f = await fixture(t, async (call, f) => {
    if (call.pathname === '/api/health' || call.pathname === '/api/screen-panel/state') {
      const body = await f.defaults(call.pathname).json();
      if (call.pathname === '/api/health') body.data.memory.degraded = true;
      else body.data.application.runtime.running = false;
      return Response.json(body);
    }
  });
  const report = await f.run();
  assert.equal(report.status, 'FAIL');
  assert.equal(report.degradedSamples, report.samples);
  assert.equal(report.runtimeFailures, report.samples);
  assert.equal(report.touchFailures, 0);
});

test('missing heap metrics cannot pass silently', async t => {
  const f = await fixture(t, call => call.pathname === '/api/health' ? ok({ memory: {} }) : undefined);
  const report = await f.run();
  assert.notEqual(report.status, 'PASS');
  assert(report.transportErrors > 0);
  assert.equal(report.samples, 0);
});

for (const phase of ['preflight', 'initialization', 'sampling', 'cleanup']) {
  test('HTTP 429 is retained during ' + phase, async t => {
    let injected = false;
    const route = { preflight: '/api/health', initialization: '/api/auth/login', sampling: '/api/system/info', cleanup: '/api/auth/logout' }[phase];
    const f = await fixture(t, call => {
      if (call.pathname === route && !injected) { injected = true; return httpError(429); }
    });
    const report = await f.run();
    assert.notEqual(report.status, 'PASS');
    assert.equal(report.http429Count, 1);
    if (phase === 'initialization') assert.equal(report.initializationFailures, 1);
    if (phase === 'cleanup') assert(report.errors.some(error => error.kind === 'cleanup'));
  });
}

for (const change of ['revision', 'config hash', 'deployment report']) {
  test(change + ' boundary cannot be combined into a successful window', async t => {
    const f = await fixture(t, async (call, f) => {
      if (change === 'revision' && call.pathname === '/api/screen-panel/state' && f.visits[call.pathname] === 2) {
        const body = await f.defaults(call.pathname).json(); body.data.revision++;
        return Response.json(body);
      }
      if (change === 'config hash' && call.pathname === '/api/screen-panel/config' && f.visits[call.pathname] === 3) {
        const body = await f.defaults(call.pathname).json(); body.data.config.pages[0].title = 'changed';
        return Response.json(body);
      }
      if (change === 'deployment report' && call.pathname === '/api/screen-panel/state' && f.visits[call.pathname] === 2) {
        await fs.writeFile(f.deploymentPath, JSON.stringify({ ...f.deployment, webSha256: 'c'.repeat(64) }));
      }
    });
    await f.plan();
    const report = await f.run();
    assert.equal(report.status, 'FAIL');
    assert.equal(report[change === 'deployment report' ? 'deploymentChanges' : 'configChanges'], 1);
  });
}

test('mismatched observation plan refuses network requests', async t => {
  const f = await fixture(t);
  await f.plan({ webSha256: 'd'.repeat(64) });
  const report = await f.run();
  assert.equal(report.status, 'FAIL');
  assert.equal(report.deploymentChanges, 1);
  assert.equal(f.calls.length, 0);
});

test('unverified deployment and existing output directory are never overwritten', async t => {
  const f = await fixture(t);
  await fs.writeFile(f.deploymentPath, JSON.stringify({ ...f.deployment, verified: false }));
  await assert.rejects(f.run(), /Verified deployment required/);
  await fs.writeFile(f.deploymentPath, JSON.stringify(f.deployment));
  await fs.mkdir(f.options.out);
  const file = path.join(f.options.out, 'report.json');
  await fs.writeFile(file, 'historical evidence');
  await assert.rejects(f.run(), { code: 'EEXIST' });
  assert.equal(await fs.readFile(file, 'utf8'), 'historical evidence');
  assert.equal(f.calls.length, 0);
});

test('slow initialization does not consume the requested observation window', async t => {
  const f = await fixture(t, (call, f) => { if (call.pathname === '/api/auth/login') f.clock += 90000; });
  const report = await f.run();
  assert.equal(report.status, 'PASS');
  assert.equal(report.actualSampleSpanSeconds, 60);
  assert(report.elapsedSeconds >= 150);
  assert(Date.parse(report.samplingStartedAt) >= EPOCH + 90000);
});

test('a single slow sample cannot impersonate a complete window', async t => {
  const f = await fixture(t, (call, f) => { if (call.pathname === '/api/system/info') f.clock += 70000; });
  const report = await f.run();
  assert.equal(report.status, 'INCOMPLETE');
  assert.equal(report.samples, 1);
  assert.equal(report.actualSampleSpanSeconds, 0);
});

test('interruptions retain evidence but never pass', async t => {
  const f = await fixture(t, (call, f) => { if (call.pathname === '/api/screen-panel/state') f.controller.abort(); });
  assert.equal((await f.run()).status, 'INCOMPLETE');
  assert.equal(f.calls.at(-1).pathname, '/api/auth/logout');
});

test('already interrupted run makes no requests', async t => {
  const f = await fixture(t); f.controller.abort();
  assert.equal((await f.run()).status, 'INCOMPLETE');
  assert.equal(f.calls.length, 0);
});

test('uptime rollback is preserved as a restart', async t => {
  const f = await fixture(t, async (call, f) => {
    if (call.pathname === '/api/system/info' && f.visits[call.pathname] >= 3) {
      const body = await f.defaults(call.pathname).json(); body.data.uptime.seconds -= 100;
      return Response.json(body);
    }
  });
  const report = await f.run();
  assert.equal(report.restarts, 1);
  assert.equal(report.status, 'FAIL');
});

test('recovered transient failures stay counted and saved errors are credential-free', async t => {
  const f = await fixture(t, (call, f) => {
    if (call.pathname === '/api/system/info' && f.visits[call.pathname] === 2) {
      throw new Error(f.credentials.username + ':' + f.credentials.password + ' Bearer offline-token-1');
    }
  });
  const report = await f.run();
  assert.equal(report.transportErrors, 1);
  assert(report.samples > 1);
  assert.notEqual(report.status, 'PASS');
  for (const name of await fs.readdir(f.options.out)) {
    const data = await fs.readFile(path.join(f.options.out, name), 'utf8');
    for (const secret of [...Object.values(f.credentials), 'offline-token-1']) assert(!data.includes(secret));
  }
});

test('overlapping username and password do not expose a password suffix', async t => {
  const f = await fixture(t, (call, f) => {
    if (call.pathname === '/api/system/info') throw new Error(f.credentials.username + ':' + f.credentials.password);
  });
  f.credentials.username = 'overlapping-secret';
  f.credentials.password = 'overlapping-secret-sensitive-suffix';
  const report = await f.run();
  assert.notEqual(report.status, 'PASS');
  for (const name of await fs.readdir(f.options.out)) {
    const data = await fs.readFile(path.join(f.options.out, name), 'utf8');
    assert(!data.includes('sensitive-suffix'));
    assert(!data.includes('overlapping-secret'));
    assert(data.includes('[redacted]'));
  }
});

test('truncated HTTP body retains route, phase and cause without turning recovery into PASS', async t => {
  const f = await fixture(t, (call, f) => {
    if (call.pathname === '/api/system/info' && f.visits[call.pathname] === 2) {
      return {ok: true, status: 200, headers: new Headers({'content-length': '500'}),
        json: async () => { throw new TypeError('terminated ' + f.credentials.password,
          {cause: {code: 'UND_ERR_SOCKET'}}); }};
    }
  });
  const report = await f.run();
  assert.equal(report.transportErrors, 1);
  assert(report.samples > 1);
  assert.notEqual(report.status, 'PASS');
  assert.equal(report.requestFailures.length, 1);
  const failure = report.requestFailures[0];
  assert.equal(failure.route, '/api/system/info');
  assert.equal(failure.phase, 'body');
  assert.equal(failure.status, 200);
  assert.equal(failure.causeCode, 'UND_ERR_SOCKET');
  assert.equal(failure.contentLength, '500');
  assert(failure.elapsedMs >= failure.headersMs);
  assert(failure.message.includes('[redacted]'));
  assert(report.errors[0].message.includes('/api/system/info [body]'));
  for (const name of await fs.readdir(f.options.out))
    assert(!(await fs.readFile(path.join(f.options.out, name), 'utf8')).includes(f.credentials.password));
});

test('header transport failure stays distinct from a truncated successful HTTP response', async t => {
  const f = await fixture(t, (call, f) => {
    if (call.pathname === '/api/system/info' && f.visits[call.pathname] === 2)
      throw new TypeError('fetch failed', {cause: {code: 'ECONNRESET'}});
  });
  const report = await f.run();
  assert.equal(report.requestFailures[0].phase, 'headers');
  assert.equal(report.requestFailures[0].status, undefined);
  assert.equal(report.requestFailures[0].causeCode, 'ECONNRESET');
  assert.equal(report.transportErrors, 1);
  assert.notEqual(report.status, 'PASS');
});

test('local neighbor subprocess failure blocks credentials and is not a successful zero-error window', async t => {
  const f = await fixture(t);
  f.dependencies.readNeighbors = async () => { throw new Error('Local neighbor lookup failed: process timeout (30000ms)'); };
  const report = await f.run();
  assert.equal(report.preflightFailures, 1);
  assert.equal(report.initializationFailures, 0);
  assert.equal(report.samples, 0);
  assert.equal(report.transportErrors, 0);
  assert.equal(report.status, 'FAIL');
  assert.deepEqual(f.posts(), []);
  assert(report.errors[0].message.includes('Local neighbor lookup failed'));
});

for (const mode of ['stale weather', 'empty history', 'stale history']) {
  test(mode + ' fails data acceptance independently of runtime', async t => {
    const f = await fixture(t, (call, f) => {
      if (mode === 'stale weather' && call.pathname === '/api/screen-panel/weather/refresh') return ok({ updatedMs: 1000, stale: true });
      if (mode !== 'stale weather' && call.pathname === '/api/periph-exec/history') return ok({ points: mode === 'empty history' ? [] : [{ seq: f.clock }], stale: mode === 'stale history' });
    });
    const report = await f.run();
    assert.equal(report.status, 'FAIL');
    assert.equal(report.stabilityStatus, 'PASS');
    assert.equal(report.dataStatus, 'FAIL');
  });
}

test('long virtual windows require weather and history advancement', async t => {
  const f = await fixture(t, call => call.pathname === '/api/periph-exec/history' ? ok({ points: [{ seq: 1 }], stale: false }) : undefined);
  const report = await f.run({ seconds: 7200, interval: 300 });
  assert.equal(report.status, 'FAIL');
  assert.equal(report.weatherUpdates, 0);
  assert.equal(report.historyUpdates['test.temperature'], 0);
  assert.equal(report.dataStatus, 'FAIL');
});

test('advancing weather and histories pass a long virtual window, not physical acceptance', async t => {
  const f = await fixture(t, (call, f) => call.pathname === '/api/screen-panel/weather/refresh' ? ok({ updatedMs: Math.floor(f.clock / 1800000) * 1800000 + 1000, stale: false }) : undefined);
  const report = await f.run({ seconds: 7200, interval: 300 });
  assert.equal(report.status, 'PASS');
  assert(report.weatherUpdates > 0);
  assert(report.historyUpdates['test.temperature'] > 0);
});
