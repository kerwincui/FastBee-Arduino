const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { test } = require('node:test');

function setup() {
  let sequence = 0, now = 0;
  const timers = new Map(), delays = [], sources = [];
  const sandbox = {
    AppState: {}, console: { warn() {}, error() {} },
    Date: { now: () => now },
    setTimeout(fn, delay) { const id = ++sequence; timers.set(id, fn); delays.push(delay); return id; },
    clearTimeout(id) { timers.delete(id); },
    EventSource: class {
      constructor() { this.closed = false; this.bindings = []; sources.push(this); }
      close() { this.closed = true; }
      addEventListener(type, callback) { this.bindings.push({ type, callback }); }
      removeEventListener() {}
    },
  };
  vm.runInNewContext(fs.readFileSync(path.resolve(__dirname, '../../web-src/js/state-sse.js'), 'utf8'), sandbox);
  const app = sandbox.AppState;
  app.onSSEEvent('modbus-data', () => {});
  return { app, timers, delays, sources, setNow(value) { now=value; }, retry() {
    const [id, fn] = timers.entries().next().value;
    timers.delete(id); fn();
  } };
}

test('failed SSE attempts back off to 30 seconds and only open resets the delay', () => {
  const state = setup();
  state.app.connectSSE();
  for (let i = 0; i < 7; i++) {
    state.app._sseSource.onerror();
    assert.equal(state.timers.size, 1);
    state.retry();
    assert.equal(state.app._sseSource.bindings.filter(x=>x.type==='modbus-data').length, 2);
  }
  assert.deepEqual(state.delays.filter(x=>x!==15000), [1000, 2000, 4000, 8000, 16000, 30000, 30000]);
  state.app._sseSource.onopen();
  state.app._sseSource.onerror();
  assert.equal(state.delays.at(-1), 1000);
});

test('explicit connect or page teardown cancels a pending retry', () => {
  const state = setup();
  state.app.connectSSE(); state.app._sseSource.onerror();
  assert.equal(state.timers.size, 1);
  state.app.connectSSE();
  assert.equal(state.timers.size, 1); // only the new connection's watchdog
  assert.equal(state.sources.length, 2);
  state.app._sseSource.onerror(); state.app.disconnectSSE();
  assert.equal(state.timers.size, 0);
  assert.equal(state.app._sseSource, null);
});

test('a failed connection with no subscribers does not reconnect', () => {
  const state = setup(); state.app._sseHandlers = {};
  state.app.connectSSE(); state.app._sseSource.onerror();
  assert.equal(state.timers.size, 0);
});

test('silent stream is closed and schedules one retry', () => {
  const state=setup(); state.app.connectSSE(); const old=state.app._sseSource;
  state.setNow(140000); state.retry();
  assert.equal(old.closed,true); assert.equal(state.timers.size,1);
  state.retry(); assert.equal(state.sources.length,2);
});

test('late callbacks from an old source cannot close or reset the new source', () => {
  const state=setup(); state.app.connectSSE(); const old=state.app._sseSource;
  old.onerror(); state.retry(); const current=state.app._sseSource;
  old.onerror(); old.onopen();
  assert.equal(state.app._sseSource,current); assert.equal(current.closed,false);
  assert.equal(state.timers.size,1);
});

test('heartbeat keeps watchdog alive and reconnect asks for snapshot recovery once', () => {
  const state=setup(); let recovered=0; state.app.onSSEEvent('resync',()=>recovered++);
  state.app.connectSSE(); const source=state.app._sseSource;
  state.setNow(120000); source.bindings.find(x=>x.type==='heartbeat').callback();
  state.setNow(140000); state.retry(); assert.equal(source.closed,false);
  source.onerror(); state.retry(); state.app._sseSource.onopen();
  assert.equal(recovered,1); state.app._sseSource.onopen(); assert.equal(recovered,1);
});
