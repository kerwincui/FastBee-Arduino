const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { minifyJS } = require('../../scripts/minify-js');
const source = fs.readFileSync('web-src/js/request-governor.js', 'utf8')
  .replace(/\}\)\(\);\s*$/, 'window.__governor = Governor;})();');
const tick = () => new Promise(resolve => setTimeout(resolve, 5));
async function until(check) {
  const deadline = Date.now() + 3000;
  while (!check()) { assert(Date.now() < deadline, 'request did not settle'); await tick(); }
}
async function probe(minified) {
  const calls = [], held = [];
  let hold = false, rejectBatch = false, failedPath = null;
  const sandbox = {
    console: { log() {}, warn() {}, error() {} }, URL, Promise, AbortController, setTimeout, clearTimeout,
    location: { hostname: 'device', origin: 'http://device' },
    localStorage: { getItem: () => null, removeItem() {}, setItem() {} },
    document: { querySelectorAll: () => [], getElementById: () => null },
    fetch: async (url, options) => {
      const path = new URL(url).pathname;
      const payload = options.body ? JSON.parse(options.body) : null;
      calls.push({ path, payload });
      if (hold) await new Promise((resolve, reject) => {
        held.push(resolve);
        options.signal.addEventListener('abort', () => {
          const error = new Error('cancelled'); error.name = 'AbortError'; reject(error);
        }, { once: true });
      });
      const requests = payload && payload.requests;
      const rejected = requests && (requests.length > 5 || rejectBatch);
      return { ok: !rejected, status: rejected ? 400 : 200, headers: { get: () => null },
        json: async () => rejected ? { success: false, message: 'Batch rejected' }
          : requests ? { success: true, results: requests.map(item => item.url === failedPath
            ? { success: false, error: 'Endpoint unavailable' }
            : { success: true, data: { path: item.url } }) }
            : { success: true, data: { path } } };
    }
  };
  sandbox.window = sandbox;
  vm.runInNewContext(minified ? minifyJS(source) : source, sandbox);
  const gov = sandbox.__governor;
  const reads = Array.from({ length: 12 }, () => sandbox.apiBatchGet('/api/system/status'));
  reads.push(sandbox.apiBatchGet('/api/network/status'));
  const results = await Promise.all(reads);
  assert.equal(calls.length, 1, 'duplicate widgets share one transport');
  assert.equal(calls[0].payload.requests.length, 2, 'duplicate status creates one JSON sub-response');
  assert.equal(results[0].data.path, '/api/system/status');
  assert.equal(results[12].data.path, '/api/network/status');
  assert.equal(Object.keys(gov._dedupMap).length, 0, 'settled entries do not leak');

  const endpoints = ['/api/system/status', '/api/system/info', '/api/health', '/api/system/health',
    '/api/network/status', '/api/network/config', '/api/device/config', '/api/device/time'];
  const before = calls.length;
  const grouped = await sandbox.apiBatchGetManyFresh(endpoints.map(url => ({ url })));
  assert.equal(grouped.length, endpoints.length);
  grouped.forEach((result, index) => assert.equal(result.data.path, endpoints[index]));
  assert.equal(calls.length - before, 2, 'eight reads use batches of five and three');
  for (const call of calls.slice(before)) assert(call.payload.requests.length <= 5);
  const burstStart = calls.length;
  await Promise.all(endpoints.map(url => sandbox.apiBatchGetFresh(url)));
  assert.equal(calls.length - burstStart, 2, 'windowed batching is bounded as well');
  for (const call of calls.slice(burstStart)) assert(call.payload.requests.length <= 5);
  failedPath = endpoints[6];
  const partial = await sandbox.apiBatchGetManyFresh(endpoints.map(url => ({ url })));
  assert.equal(partial[6].success, false, 'sub-response error keeps its original index');
  assert.equal(partial[7].data.path, endpoints[7], 'later sub-response remains available');
  failedPath = null;
  rejectBatch = true;
  const rejectedStart = calls.length;
  await assert.rejects(sandbox.apiBatchGetManyFresh(endpoints.map(url => ({ url }))));
  assert.equal(calls.length - rejectedStart, 1, 'HTTP failure does not retry or send remaining chunks');
  rejectBatch = false;

  hold = true;
  const old = sandbox.apiGetFresh('/api/system/status');
  const cancelled = assert.rejects(old, error => error._pageAborted === true);
  await until(() => held.length === 1);
  gov.abortPageRequests();
  const current = sandbox.apiGetFresh('/api/system/status');
  await cancelled;
  await until(() => held.length === 2);
  await tick();
  const duplicate = sandbox.apiGetFresh('/api/system/status');
  assert.equal(duplicate, current, 'old completion must not erase the next page dedup entry');
  held[1]();
  await Promise.all([current, duplicate]);
  await until(() => gov._inflight === 0);
  assert.equal(Object.keys(gov._dedupMap).length, 0);
  held.length = 0;
  const navigationStart = calls.length;
  const group = sandbox.apiBatchGetManyFresh(endpoints.map(url => ({ url })));
  const groupCancelled = assert.rejects(group, error => error._pageAborted === true);
  await until(() => held.length === 1);
  gov.abortPageRequests();
  await groupCancelled;
  await until(() => gov._inflight === 0);
  assert.equal(calls.length - navigationStart, 1, 'navigation prevents subsequent chunks from transmitting');
}
async function probeSerial(minified) {
  const calls = [];
  let active = 0, peak = 0, fail = false, hold = false, entered = false;
  const sandbox = {
    console: { log() {}, warn() {}, error() {} }, URL, Promise, AbortController, setTimeout, clearTimeout,
    location: { hostname: 'device', origin: 'http://device' },
    localStorage: { getItem: () => null, removeItem() {}, setItem() {} },
    document: { querySelectorAll: () => [], getElementById: () => null },
    fetch: async (url, options) => {
      calls.push({ url, method: options.method });
      active++; peak = Math.max(peak, active);
      try {
        if (hold) await new Promise((resolve, reject) => {
          entered = true;
          options.signal.addEventListener('abort', () => {
            const error = new Error('cancelled'); error.name = 'AbortError'; reject(error);
          }, { once: true });
        });
        await tick();
        return { ok: !fail, status: fail ? 503 : 200, headers: { get: () => null },
          json: async () => fail ? { success: false, message: 'low memory' }
            : { success: true, data: { path: new URL(url).pathname } } };
      } finally { active--; }
    }
  };
  sandbox.window = sandbox;
  const code = source.replace('maxConcurrent: 2', 'maxConcurrent: 1');
  vm.runInNewContext(minified ? minifyJS(code) : code, sandbox);
  const endpoints = [{ url: '/api/system/info' }, { url: '/api/network/status' }];
  const results = await sandbox.apiBatchGetMany(endpoints);
  assert.equal(peak, 1);
  assert.equal(calls.length, 2);
  assert(calls.every(call => call.method === 'GET' && !call.url.includes('/api/batch')));
  results.forEach((result, index) => assert.equal(result.data.path, endpoints[index].url));
  await sandbox.apiBatchGetMany(endpoints);
  assert.equal(calls.length, 2, 'serial groups reuse endpoint cache');
  await sandbox.apiBatchGetManyFresh(endpoints);
  assert.equal(calls.length, 4, 'manual refresh bypasses cache without batching');
  await until(() => sandbox.__governor._inflight === 0);
  hold = true;
  const group = sandbox.apiBatchGetManyFresh(endpoints);
  const cancelled = assert.rejects(group, error => error._pageAborted === true);
  await until(() => entered);
  sandbox.__governor.abortPageRequests();
  await cancelled;
  await until(() => sandbox.__governor._inflight === 0);
  assert.equal(calls.length, 5, 'navigation prevents second serial GET');
  hold = false; fail = true;
  await assert.rejects(sandbox.apiBatchGetManyFresh(endpoints), error => error.status === 503);
  await until(() => sandbox.__governor._inflight === 0);
  assert.equal(calls.length, 6, '503 does not fan out, retry, or request later endpoints');
}
(async () => {
  await probe(false);
  await probe(true);
  await probeSerial(false);
  await probeSerial(true);
  console.log('batch deduplication, server limit, result order and navigation race passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
