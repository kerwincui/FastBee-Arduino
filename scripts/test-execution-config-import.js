'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const crypto = require('node:crypto');

async function scenario(mode) {
    const state = { registerModule(name, methods) { Object.assign(this, methods); } };
    const calls = [];
    const delays = [];
    let retried = false;
    const window = { crypto: crypto.webcrypto, filterConfigFields() { throw new Error('must not filter managed config'); } };
    const sandbox = { window, AppState: state, console, Uint8Array,
        setTimeout(fn, delay) { delays.push(delay); fn(); }, document: { getElementById() { return null; }, querySelector() { return null; } },
        apiPost: async (url, payload) => {
            calls.push({ url, ...payload });
            assert.equal(url, '/api/periph-exec/import-chunk');
            const end = Number(payload.offset) + Buffer.byteLength(payload.chunk);
            if (mode === 'deny') throw Object.assign(new Error('rejected'), { status: 422 });
            if (mode === 'busy' && !retried) {
                retried = true;
                throw Object.assign(new Error('pressure'), { status: 503, data: { error: 'CONFIG_MEMORY_PRESSURE', retryAfter: 5 } });
            }
            if (mode === 'network' && !retried && Number(payload.offset) > 0) {
                retried = true;
                throw new Error('lost response after write');
            }
            return { success: true, data: { received: mode === 'mismatch' ? end + 1 : end,
                saved: end === Number(payload.size), restartRequired: end === Number(payload.size),
                applied: mode === 'wrong-applied' } };
        }
    };
    vm.createContext(sandbox);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../web-src/modules/runtime/device-config.js'), 'utf8'), sandbox);
    const content = JSON.stringify({ rules: [{ id: 'different-from-current', name: '中文😀'.repeat(500), actions: [] }] });
    const result = state._importConfigTransferEntry({ name: 'periph_exec.json', content });
    if (['deny', 'mismatch', 'wrong-applied'].includes(mode)) {
        await assert.rejects(result);
        if (mode === 'deny') assert.equal(calls.length, 1);
        return;
    }
    assert.equal((await result).restartRequired, true);
    assert.equal(new Set(calls.map(c => c.session)).size, 1);
    assert.match(calls[0].session, /^[a-f0-9]{32}$/);
    const unique = calls.filter((c, i) => i === 0 || c.offset !== calls[i - 1].offset);
    assert.equal(unique.map(c => c.chunk).join(''), content);
    let offset = 0;
    for (const call of unique) {
        assert.equal(Number(call.offset), offset);
        assert.ok(Buffer.byteLength(call.chunk) <= 1536);
        offset += Buffer.byteLength(call.chunk);
    }
    assert.equal(offset, Buffer.byteLength(content));
    if (mode === 'busy') assert.equal(delays[0], 5000);
    if (mode === 'network') {
        const duplicate = calls.findIndex((c, i) => i && c.offset === calls[i - 1].offset);
        assert.ok(duplicate > 0);
        assert.deepEqual(calls[duplicate], calls[duplicate - 1]);
    }
}
(async () => {
    for (const mode of ['success', 'network', 'busy', 'deny', 'mismatch', 'wrong-applied']) await scenario(mode);
    console.log('Execution configuration import: 6 behavior scenarios passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
