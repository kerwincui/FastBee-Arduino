const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const { minifyJS } = require('../../scripts/minify-js');
const source = fs.readFileSync('web-src/js/request-governor.js', 'utf8');
const index = fs.readFileSync('web-src/index.html', 'utf8');
const queueStart = index.indexOf('window.__fastbeeAssetVersion');
const queueSource = index.slice(queueStart, index.indexOf('</script>', queueStart));
const tick = () => new Promise(resolve => setTimeout(resolve, 10));
const deferred = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };
async function until(check) {
  const deadline = Date.now() + 3000;
  while (!check()) { assert(Date.now() < deadline, 'transport stalled'); await tick(); }
}
async function probe(minified, concurrent) {
  const calls = [];
  let responseBody = Promise.resolve({ success: true, data: {} });
  const sandbox = {
    console: { log() {}, warn() {}, error() {} }, URL, Promise, AbortController, setTimeout, clearTimeout,
    location: { hostname: 'device', origin: 'http://device' },
    localStorage: { getItem: () => null, removeItem() {}, setItem() {} },
    document: { querySelectorAll: () => [], getElementById: () => null },
    fetch: async url => { calls.push(url); return { ok: true, status: 200, json: () => responseBody }; }
  };
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(queueSource, sandbox);
  const code = source.replace(/maxConcurrent: 2/, 'maxConcurrent: ' + concurrent)
    .replace(/\}\)\(\);\s*$/, 'window.__governor = Governor;})();');
  vm.runInContext(minified ? minifyJS(code) : code, sandbox);
  const asset = deferred();
  let assetStarted = false;
  const assetRun = sandbox.__fastbeeQueueStaticAsset(() => { assetStarted = true; return asset.promise; });
  await until(() => assetStarted);
  const body = deferred(); responseBody = body.promise;
  const api = sandbox.apiGet('/api/features/status');
  await tick();
  assert.equal(calls.length, concurrent === 1 ? 0 : 1, 'constrained API waits for the active asset');
  asset.resolve(); await assetRun;
  await until(() => calls.length === 1);
  if (concurrent === 1) {
    let nextAssetStarted = false;
    const nextAsset = sandbox.__fastbeeQueueStaticAsset(() => { nextAssetStarted = true; });
    await tick();
    assert.equal(nextAssetStarted, false, 'API retains transport until JSON body is consumed');
    body.resolve({ success: true, data: {} }); await api; await nextAsset;

    const blocked = deferred();
    const blocker = sandbox.__fastbeeQueueStaticAsset(() => blocked.promise);
    const pending = sandbox.apiGet('/api/protocol/config');
    const cancelled = assert.rejects(pending, error => error._pageAborted === true);
    sandbox.__governor.abortPageRequests();
    blocked.resolve(); await blocker; await cancelled;
    assert.equal(calls.length, 1, 'navigation cancelled queued API without touching device');

    await assert.rejects(sandbox.__fastbeeQueueStaticAsset(() => Promise.reject(new Error('asset failed'))));
    responseBody = Promise.resolve({ success: true, data: {} });
    await sandbox.apiGet('/api/health');
    assert.equal(calls.length, 2, 'failed asset does not poison subsequent API traffic');
  } else {
    body.resolve({ success: true, data: {} }); await api;
  }
}
(async () => {
  for (const minified of [false, true]) for (const concurrent of [1, 2]) await probe(minified, concurrent);
  console.log('shared static/API budget, body lifetime, navigation cancellation and recovery passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
