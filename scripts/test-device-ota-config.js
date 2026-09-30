'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../web-src/modules/runtime/device-ota.js'), 'utf8');

function fixture(options = {}) {
    const elements = new Map();
    const get = id => {
        if (!elements.has(id)) elements.set(id, {
            value: '', disabled: false, textContent: '', className: '',
            classList: { toggle() {}, add() {}, remove() {} },
            addEventListener() {}, focus() {},
        });
        return elements.get(id);
    };
    const requests = [], messages = [];
    const AppState = { registerModule(_name, methods) { Object.assign(this, methods); }, onPageHide() {} };
    const sandbox = {
        AppState, URL, console, window: {}, document: { getElementById: get },
        setTimeout: () => 0, clearTimeout() {}, setInterval: () => 0, clearInterval() {},
        confirm: () => true,
        apiGet: async url => {
            requests.push({ method: 'GET', url });
            if (url.includes('capabilities')) return { success: true, data: { ota: options.supported !== false } };
            if (url.includes('section=ota')) {
                if (options.loadFailure) throw new Error('read failed');
                return { success: true, data: { otaBaseUrl: options.base || '' } };
            }
            return { success: true, data: {} };
        },
        apiPost: async (url, body) => {
            requests.push({ method: 'POST', url, body: JSON.parse(JSON.stringify(body)) });
            return options.post ? options.post() : { success: true };
        },
    };
    vm.runInNewContext(source, sandbox);
    AppState._showOtaMessage = (id, ok, text) => messages.push({ id, ok, text });
    return { app: AppState, get, requests, messages };
}

test('loads legacy HTTPS/HTTP bases in the OTA editor and saves only that field', async () => {
    for (const base of ['https://platform.example/fw', 'http://192.168.5.2:8080/releases']) {
        const f = fixture({ base });
        await f.app._loadOtaConfig();
        assert.equal(f.get('ota-base-url').value, base);
        assert.equal(f.get('ota-config-save-btn').disabled, false);
        f.get('ota-base-url').value = ' http://192.168.5.3:8000/fw ';
        await f.app.saveOtaConfig();
        assert.deepEqual(f.requests.at(-1), {
            method: 'POST', url: '/api/protocol/config',
            body: { mqtt_otaBaseUrl: 'http://192.168.5.3:8000/fw' },
        });
        assert.equal(f.messages.at(-1).ok, true);
    }
});

test('empty value can explicitly restore the HTTPS fallback', async () => {
    const f = fixture({ base: 'http://lan/fw' });
    await f.app._loadOtaConfig();
    f.get('ota-base-url').value = '';
    await f.app.saveOtaConfig();
    assert.deepEqual(f.requests.at(-1).body, { mqtt_otaBaseUrl: '' });
});

test('failed or unfinished load cannot silently overwrite saved settings', async () => {
    const f = fixture({ loadFailure: true });
    await f.app.saveOtaConfig();
    await f.app._loadOtaConfig();
    await f.app.saveOtaConfig();
    assert.equal(f.get('ota-config-save-btn').disabled, true);
    assert.equal(f.requests.filter(r => r.method === 'POST').length, 0);
    assert.equal(f.messages.at(-1).ok, false);
});

test('rejects invalid base URLs without making save requests', async () => {
    const f = fixture();
    await f.app._loadOtaConfig();
    for (const value of ['ftp://host', 'https://', 'http://user:pw@host', 'http://@host',
        'https://host?x=1', 'http://host#part', 'http://host/ space',
        'http://host/\\path', 'http://host:0', 'http://host:70000', 'https://' + 'x'.repeat(513)]) {
        f.get('ota-base-url').value = value;
        await f.app.saveOtaConfig();
    }
    assert.equal(f.requests.filter(r => r.method === 'POST').length, 0);
});

test('manual download accepts HTTP/HTTPS and signed query URLs', () => {
    const f = fixture();
    for (const value of ['https://host/fw.bin', 'http://host:8080/fw.bin', 'https://host/fw.bin?token=a']) {
        assert.equal(f.app._isValidOtaUrl(value, false), true);
    }
    assert.equal(f.app._isValidOtaUrl('https://host/fw.bin#fragment', false), false);
});

test('saving blocks duplicate submissions and reports server failure', async () => {
    let finish;
    const f = fixture({ post: () => new Promise(resolve => { finish = resolve; }) });
    await f.app._loadOtaConfig();
    f.get('ota-base-url').value = 'http://lan';
    const saving = f.app.saveOtaConfig();
    await f.app.saveOtaConfig();
    assert.equal(f.get('ota-config-save-btn').disabled, true);
    assert.equal(f.requests.filter(r => r.method === 'POST').length, 1);
    finish({ success: false, error: 'write failed' });
    await saving;
    assert.equal(f.messages.at(-1).ok, false);
    assert.equal(f.get('ota-config-save-btn').disabled, false);
});

test('unsupported firmware never requests OTA settings or status', async () => {
    const f = fixture({ supported: false });
    f.app.refreshOtaInfo();
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(f.requests.some(r => r.url.includes('section=ota') || r.url === '/api/ota/status'), false);
    assert.equal(f.get('ota-config-save-btn').disabled, true);
});

test('active upgrades block settings changes', async () => {
    const f = fixture();
    await f.app._loadOtaConfig();
    f.app._otaBusy = true;
    f.app._updateButtonStates();
    await f.app.saveOtaConfig();
    assert.equal(f.get('ota-config-save-btn').disabled, true);
    assert.equal(f.requests.filter(r => r.method === 'POST').length, 0);
});
