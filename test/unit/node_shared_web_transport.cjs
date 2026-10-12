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
async function fallbackProbe(minified, outcome) {
  const start = index.lastIndexOf('(function() {', index.indexOf('function setSubmitState'));
  assert(start >= 0, 'fallback guard found');
  const guard = index.slice(start, index.indexOf('</script>', start))
    .replace('var bootLoginWaitMs = 15000;', 'var bootLoginWaitMs = 0;');
  const body = deferred();
  const asset = deferred();
  const calls = [], redirects = [], timers = new Map();
  let submit, failures = 0;
  const button = { innerHTML: 'Login', disabled: false };
  const form = { querySelector: () => button, addEventListener: (name, fn) => { submit = fn; } };
  const elements = { 'login-form': form, username: {value:'tester'}, password: {value:'secret'}, remember: {checked:false} };
  const sandbox = {
    console: { warn() {}, error() { failures++; } }, Promise, AbortController, Date,
    setTimeout: (fn, ms) => { if (ms === 15000) { const id = {}; timers.set(id, fn); return id; } return setTimeout(fn, ms); },
    clearTimeout: id => { if (!timers.delete(id)) clearTimeout(id); },
    document: { readyState:'complete', getElementById: id => elements[id] },
    location: { replace: url => redirects.push(url) },
    localStorage: { removeItem() {}, setItem() {} },
    fetch: async (url, options) => {
      calls.push({url,options});
      options.signal.addEventListener('abort', () => body.resolve(Promise.reject(new Error('aborted'))));
      return { ok: outcome !== 'http-error', json: () => body.promise };
    }
  };
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(queueSource, sandbox);
  vm.runInContext(minified ? minifyJS(guard) : guard, sandbox);
  let assetStarted = false;
  const assetRun = sandbox.__fastbeeQueueStaticAsset(() => { assetStarted = true; return asset.promise; });
  await until(() => assetStarted);
  submit({preventDefault(){}});
  await tick();
  assert.equal(calls.length, 0, 'expired boot wait cannot overlap active download');
  assert.equal(button.disabled, true);
  asset.resolve(); await assetRun;
  await until(() => calls.length === 1);
  assert.equal(calls[0].options.credentials, 'include');
  let nextStarted = false;
  const next = sandbox.__fastbeeQueueStaticAsset(() => { nextStarted = true; });
  await tick();
  assert.equal(nextStarted, false, 'fallback retains budget through complete JSON body');
  if (outcome === 'timeout') [...timers.values()][0]();
  else if (outcome === 'bad-json') body.resolve(Promise.reject(new SyntaxError('truncated body')));
  else body.resolve({success: outcome !== 'http-error', error:'denied'});
  await next;
  assert.equal(timers.size, 0, 'deadline cleared on success and failure');
  assert.equal(redirects.length, outcome === 'success' ? 1 : 0);
  assert.equal(failures, outcome === 'success' ? 0 : 1);
  if (outcome !== 'success') assert.equal(button.disabled, false, 'failure permits another user attempt');
}
async function cancellationSettlement(minified) {
    const callbacks = new Map(); let aborts=0, next=0, executed=0, release;
    const body = new Promise(resolve => { release=resolve; });
    const sandbox = {Promise, AbortController, console,
        setTimeout(fn,ms){if(ms===8000){const id={};callbacks.set(id,fn);return id;}return setTimeout(fn,ms);},
        clearTimeout(id){if(!callbacks.delete(id))clearTimeout(id);},
        fetch:async(url,options)=>{options.signal.addEventListener('abort',()=>{aborts++;});return {ok:true,headers:{get:()=> 'application/javascript'},text:()=>body};},
        document:{currentScript:null,createElement:()=>({remove(){}}),head:{appendChild(){executed++;}}},
        addEventListener(){},removeEventListener(){}
    };sandbox.window=sandbox;vm.createContext(sandbox);vm.runInContext(minified?minifyJS(queueSource):queueSource,sandbox);
    const failed = sandbox.__fastbeeQueueStaticAsset(()=>sandbox.__fastbeeFetchBootScript('/js/chunk-1-core.js'));
    const rejected = assert.rejects(failed,/expired/);
    const queued = sandbox.__fastbeeQueueStaticAsset(()=>{next++;});
    await until(()=>callbacks.size===1);[...callbacks.values()][0]();await new Promise(resolve=>setTimeout(resolve,30));
    assert.equal(aborts,1);assert.equal(next,0,'cannot release while original transport is unsettled');
    release('window.late=1;');await rejected;await queued;
    assert.equal(next,1);assert.equal(executed,0);assert.equal(callbacks.size,0);

}

async function imageBodyProbe(minified, expire) {
  let resolveBody, rejectBody, calls = 0, objectUrls = 0, next = 0;
  const body = new Promise((resolve, reject) => { resolveBody = resolve; rejectBody = reject; });
  const timers = new Map(), displayed = [];
  const sandbox = {
    Promise, AbortController,
    URL: { createObjectURL(value) { assert.equal(value, 'complete-blob'); objectUrls++; return 'blob:fixture'; } },
    setTimeout(fn, ms) { if (ms === 8000) { const id = {}; timers.set(id, fn); return id; } return setTimeout(fn, ms); },
    clearTimeout(id) { if (!timers.delete(id)) clearTimeout(id); },
    document: { currentScript: null },
    fetch: async (url, options) => {
      calls++;
      options.signal.addEventListener('abort', () => rejectBody(new Error('aborted')));
      return { ok: true, blob: () => body };
    }
  };
  sandbox.window = sandbox; vm.createContext(sandbox);
  vm.runInContext(minified ? minifyJS(queueSource) : queueSource, sandbox);
  const element = { tagName: 'IMG', setAttribute(name, value) { displayed.push({name, value}); } };
  const image = sandbox.__fastbeeLoadImageAsset(element, '/logo.png');
  const failure = expire ? assert.rejects(image, /aborted/) : null;
  await until(() => calls === 1);
  const queued = sandbox.__fastbeeQueueStaticAsset(() => { next++; });
  await tick(); assert.equal(next, 0, 'image retains slot while blob body is incomplete');
  if (expire) [...timers.values()][0](); else resolveBody('complete-blob');
  if (failure) await failure; else await image;
  await queued;
  assert.equal(timers.size, 0); assert.equal(objectUrls, expire ? 0 : 1);
  if (expire) assert.equal(sandbox.__fastbeeImageAssets['/logo.png'], undefined);
  else {
    await sandbox.__fastbeeLoadImageAsset(element, '/logo.png');
    assert.equal(calls, 1, 'completed image cache prevents another device transfer');
    assert.equal(displayed.length, 2);
  }
}
(async () => {
  for (const minified of [false, true]) for (const expire of [false, true]) await imageBodyProbe(minified, expire);
  for (const minified of [false, true]) await cancellationSettlement(minified);
  for (const minified of [false, true]) for (const concurrent of [1, 2]) await probe(minified, concurrent);
  for (const minified of [false, true]) for (const outcome of ['success', 'http-error', 'bad-json', 'timeout']) await fallbackProbe(minified, outcome);
  console.log('shared static/API budget, body lifetime, navigation cancellation and recovery passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
