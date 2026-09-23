const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
let source = fs.readFileSync('web-src/js/request-governor.js', 'utf8');
const closing = source.lastIndexOf('})();');
source = source.slice(0, closing) + '\nwindow.__governor = Governor;\n' + source.slice(closing);
let now = 1000, timerId = 0, rejectCatalog = false;
const timers = new Map(), calls = [];
const sandbox = {
  console: { log() {}, warn() {}, error() {} },
  URL, Promise, AbortController,
  Date: class extends Date { static now() { return now; } },
  setTimeout: (fn, ms) => { const id = ++timerId; timers.set(id, { fn, at: now + ms }); return id; },
  clearTimeout: id => timers.delete(id),
  location: { hostname: 'device', origin: 'http://device' },
  localStorage: { getItem: () => null, removeItem() {}, setItem() {} },
  document: { querySelectorAll: () => [], getElementById: () => null },
  Notification: { warning() {}, error() {} },
  fetch: async (url, options) => {
    const route = new URL(url).pathname;
    calls.push({ route, method: options.method, at: now });
    const rejected = rejectCatalog && route === '/api/peripherals/types';
    return { ok: !rejected, status: rejected ? 503 : 200,
      headers: { get: key => rejected && key === 'Retry-After' ? '5' : '' },
      json: async () => rejected ? { success: false, message: 'Server low memory, request rejected' }
        : { success: true, data: { catalog: [1, 2, 3] } } };
  }
};
sandbox.window = sandbox;
vm.runInNewContext(source, sandbox);
const settle = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  await sandbox.apiGet('/api/peripherals/types');
  await sandbox.apiGet('/api/peripherals/types');
  assert.equal(calls.filter(c => c.route === '/api/peripherals/types').length, 1);
  await sandbox.apiPost('/api/peripherals', { id: 'changed' });
  await sandbox.apiGet('/api/peripherals/types');
  assert.equal(calls.filter(c => c.route === '/api/peripherals/types').length, 2, 'peripheral write invalidates catalog');
  now += 60001;
  await sandbox.apiGet('/api/peripherals/types');
  assert.equal(calls.filter(c => c.route === '/api/peripherals/types').length, 3, 'TTL expiration refetches catalog');

  sandbox.apiInvalidateCache('/api/peripherals/types');
  rejectCatalog = true;
  await assert.rejects(sandbox.apiGet('/api/peripherals/types'));
  await settle();
  const cooldownStart = calls.length;
  const read = sandbox.apiGet('/api/system/status'); // built-in priority 3
  const catalog = sandbox.apiGet('/api/peripherals/types');
  const batch = sandbox.__governor.enqueue('POST', '/api/batch', { priority: 3 });
  const control = sandbox.apiPostPriority('/api/modbus/coil/control', { value: true });
  const login = sandbox.apiPost('/api/auth/login', { username: 'test' });
  await Promise.all([control, login]);
  await settle();
  const bypasses = calls.slice(cooldownStart).map(c => c.route).sort();
  assert.deepEqual(bypasses, ['/api/auth/login', '/api/modbus/coil/control']);
  assert.equal(sandbox.__governor._queue.length, 3, 'all reads, including POST batch, await recovery');
  rejectCatalog = false;
  now += 5001;
  for (const [id, timer] of [...timers]) {
    if (timer.at <= now) { timers.delete(id); timer.fn(); }
  }
  await Promise.all([read, catalog, batch]);
  assert.equal(sandbox.__governor._queue.length, 0);
  assert.equal(calls.slice(cooldownStart).filter(c => c.at < now).length, 2);
  console.log('catalog cache, write invalidation, TTL and overload cooldown behavior passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
