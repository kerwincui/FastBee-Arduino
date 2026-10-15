const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const http = require('node:http');
const { minifyJS } = require('../../scripts/minify-js');
const index = fs.readFileSync('web-src/index.html', 'utf8');
const start = index.indexOf('window.__fastbeeAssetVersion');
const queue = index.slice(start, index.indexOf('</script>', start));
const loader = fs.readFileSync('web-src/js/page-loader.js', 'utf8');
const tick = ms => new Promise(resolve => setTimeout(resolve, ms));
async function probe(minified) {
  const calls = {};
  const server = http.createServer((req, res) => {
    calls[req.url] = (calls[req.url] || 0) + 1;
    res.writeHead(200, {'Content-Type':'text/html'});
    res.flushHeaders(); // No Content-Length: exercise the actual chunked path.
    if (req.url === '/hang' || req.url === '/cancel') return;
    if (req.url === '/truncate' && calls[req.url] === 1) {
      res.write('<partial>'); setTimeout(() => res.destroy(), 10); return;
    }
    setTimeout(() => res.end('<div>complete</div>'), 25);
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const base = 'http://127.0.0.1:' + server.address().port;
  // index.html's inline queue now reads document.currentScript (boot nonce);
  // stub just enough DOM for the vm sandbox. Keep vm execution inside try so
  // a setup error closes the server and the process exits instead of hanging.
  const sandbox = {console, Promise, AbortController, URL, fetch,
    document: {currentScript: null},
    setTimeout(fn, ms) { return setTimeout(fn, ms === 8000 ? 150 : ms >= 800 ? 1 : ms); }, clearTimeout};
  sandbox.window = sandbox; vm.createContext(sandbox);
  try {
    vm.runInContext(minified ? minifyJS(queue + '\n' + loader) : queue + '\n' + loader, sandbox);
    const page = sandbox.PageLoader;
    let released = false;
    const active = page._enqueueOp(() => page._fetchWithRetry(base + '/ok', 0));
    const following = page._enqueueOp(() => { released = true; });
    await tick(12); assert.equal(released, false, 'queue cannot release on headers');
    assert.equal(await active, '<div>complete</div>'); await following;
    assert.equal(await page._fetchWithRetry(base + '/truncate', 1), '<div>complete</div>');
    assert.equal(calls['/truncate'], 2, 'body failure must be inside retry scope');
    const stalled = page._enqueueOp(() => page._fetchWithRetry(base + '/hang', 0));
    const failed = assert.rejects(stalled, /timed out/);
    let afterTimeout = false;
    const resumed = page._enqueueOp(() => { afterTimeout = true; });
    await failed; await resumed; assert(afterTimeout, 'timeout must release shared queue');
    const controller = new AbortController();
    const cancelled = page._enqueueOp(() => page._fetchWithRetry(base + '/cancel', 3, controller.signal));
    const cancellation = assert.rejects(cancelled, e => e.name === 'AbortError');
    await tick(120); controller.abort(); await cancellation;
    assert.equal(calls['/cancel'], 1, 'caller cancellation never retries');
    const already = new AbortController(); already.abort();
    await assert.rejects(page._fetchWithRetry(base + '/never', 3, already.signal), e => e.name === 'AbortError');
    assert.equal(calls['/never'], undefined);
  } finally { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); }
}
(async () => {
  for (const minified of [false, true]) await probe(minified);
  console.log('chunked full-body deadline, truncated retry, queue recovery and cancellation passed');
})().catch(error => {console.error(error);process.exitCode=1;});
