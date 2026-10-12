// Real production modal methods, fake transport/DOM only. Prepared, not device evidence.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
let app, calls = [], handler, developer = true;
const nodes = new Map();
for (const name of ['state', 'review', 'confirm', 'channel', 'mode', 'value',
    'prepare-btn', 'start-btn', 'set-btn', 'enable-btn', 'disable-btn', 'stop-btn']) {
    nodes.set('peripheral-pca9685-' + name, {value: '', checked: false, disabled: false, textContent: ''});
}
const node = name => nodes.get('peripheral-pca9685-' + name);
const status = state => ({state, outputsEnabled: state === 'enabled', physicalOutputVerified: false,
    failures: 0, firstError: '', lastError: '', frequencyCalculatedHz: 50});
const response = state => ({success: true, data: {id: 'pwm-a', statusAvailable: true, status: status(state)}});
const context = {
    AppState: {registerModule(name, methods) { app = methods; }},
    document: {getElementById: id => nodes.get(id) || null},
    apiPost: async (url, payload) => { calls.push({url, payload}); return handler(url, payload); },
    apiGet: async (url, payload) => { calls.push({url, payload}); return handler(url, payload); },
    confirm: () => true, clearInterval, setInterval, Date, TextEncoder, window: {}
};
vm.runInNewContext(fs.readFileSync(path.join(root, 'web-src/js/utils.js'), 'utf8'), context);
vm.runInNewContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime/peripheral-io.js'), 'utf8'), context);
Object.assign(app, {_periphIoKind: 'pca9685', _periphIoType: 57, _periphIoId: 'pwm-a',
    _periphIoSession: {}, _periphIoBusy: false, _periphIoBlocked: '',
    isDeveloperModeEnabled: () => developer, getDeveloperModeDisabledText: () => 'disabled',
    _setPeripheralIoError(value) { this.error = value; }});
const tests = {
    async control_conflict() {
        handler = async () => ({success: false, error: 'CONTROL_CONFLICT', data: {id: 'pwm-a',
            control: {evaluated: true, reason: 'CONTROL_CONFLICT', targetId: 'pwm-a', source: 'rule',
                generation: '18446744073709551615', holdRemainingMs: 1001}}});
        await app.runPeripheralPca9685('enable');
        for (const text of ['占用', '规则', 'pwm-a', '2秒', '物理负载', '不会自动重试'])
            assert(app.error.includes(text), text + ': ' + app.error);
        assert.equal(calls.length, 1);
        assert.equal(calls[0].payload.confirmOverride, undefined);
        assert.equal(calls[0].payload.actor, undefined);
        assert.equal(app._periphPcaStatus, null);
    },
    async transport_control_rejected() {
        handler = async () => { throw {data: {success: false, data: {id: 'pwm-a', control: {
            evaluated: true, reason: 'CONTROL_STOP_UNCONFIRMED', targetId: 'pwm-a', source: 'http'}}}}; };
        await app.runPeripheralPca9685('enable');
        assert(app.error.includes('停止尚未确认') && app.error.includes('网页会话'));
        assert.equal(calls.length, 1);
    },
    async foreign_control_rejected() {
        handler = async () => { throw {data: {success: false, error: '身份未确认', data: {id: 'other', control: {
            evaluated: true, reason: 'CONTROL_CONFLICT', targetId: 'other', source: 'rule'}}}}; };
        await app.runPeripheralPca9685('enable');
        assert(!app.error.includes('规则') && !app.error.includes('other'));
        assert(app.error.includes('身份未确认'));
        assert.equal(calls.length, 1);
    },
    async foreign_nested_control_rejected() {
        handler = async () => { throw {data: {success: false, error: '身份未确认', data: {id: 'pwm-a', control: {
            evaluated: true, reason: 'CONTROL_CONFLICT', targetId: 'other', source: 'rule'}}}}; };
        await app.runPeripheralPca9685('enable');
        assert(!app.error.includes('规则') && !app.error.includes('other'));
        assert(app.error.includes('身份未确认'));
        assert.equal(calls.length, 1);
    },
    async fresh_review() {
        handler = async () => ({...response('awaiting_power_cycle'), data: {...response('awaiting_power_cycle').data,
            review: {id: 'pwm-a', token: 'a'.repeat(32), validForMs: 60000, oePadHigh: true,
                physicalPowerVerified: false, outputsEnabled: false, params: {channelMask: 1}}}});
        await app.runPeripheralPca9685('prepare');
        assert.equal(calls.length, 1); assert(app._periphPcaReview);
        assert.equal(node('start-btn').disabled, true);
        node('confirm').checked = true; app._updatePca9685Controls();
        assert.equal(node('start-btn').disabled, false);
        handler = async () => response('waking');
        await app.runPeripheralPca9685('start');
        assert.equal(calls[1].payload.token, 'a'.repeat(32));
        assert.equal(app._periphPcaReview, null);
        assert.equal(node('enable-btn').disabled, true);
        await app.runPeripheralPca9685('start'); assert.equal(calls.length, 2);
    },
    async unknown_start() {
        app._periphPcaReview = {token: 'b'.repeat(32), expires: Date.now() + 60000};
        node('confirm').checked = true;
        handler = async () => { throw new Error('lost response'); };
        await app.runPeripheralPca9685('start');
        assert.equal(calls.length, 1); assert.equal(app._periphPcaReview, null);
        assert.equal(app._periphPcaStatus, null); assert(app.error.includes('不自动重试'));
        assert.equal(node('stop-btn').disabled, false);
    },
    async late_response() {
        let resolve;
        handler = () => new Promise(r => resolve = r);
        const work = app.runPeripheralPca9685('prepare');
        app._periphIoSession = {}; app._periphIoId = 'pwm-b'; app.error = 'new'; app._periphIoBusy = true;
        resolve(response('enabled')); await work;
        assert.equal(app.error, 'new'); assert.equal(app._periphIoBusy, true);
        assert.equal(app._periphPcaReview, null); assert.equal(calls.length, 1);
    },
    async readonly_refresh() {
        handler = async () => ({success: true, data: {id: 'pwm-a', pca9685: status('ready_disabled')}});
        await app.refreshPeripheralIo(true);
        assert.equal(calls.length, 1); assert.equal(calls[0].url, '/api/peripherals/status');
        assert.equal(node('enable-btn').disabled, false);
    },
    async malformed_status() {
        for (const value of [null, {}, {state: 'enabled'}, {...status('ready_disabled'), outputsEnabled: true},
            {...status('enabled'), physicalOutputVerified: true}, status('invented')]) {
            app._renderPeripheralPca9685(value); assert.equal(app._periphPcaStatus, null);
            assert.equal(node('enable-btn').disabled, true);
        }
        assert.equal(calls.length, 0);
    },
    async wrong_identity() {
        handler = async () => ({success: true, data: {id: 'pwm-b', statusAvailable: true, status: status('enabled')}});
        await app.runPeripheralPca9685('enable');
        assert(app.error.includes('身份')); assert.equal(app._periphPcaStatus, null);
    },
    async literal_input() {
        handler = async () => response('ready_disabled');
        node('mode').value = 'pulse'; node('channel').value = '15'; node('value').value = '1500';
        await app.runPeripheralPca9685('set');
        assert.deepEqual(JSON.parse(JSON.stringify(calls[0].payload)), {id: 'pwm-a', action: 'pulse', channel: 15, value: 1500});
        for (const text of ['', '16', '-1', '0.5', '0junk']) {
            node('channel').value = text; await app.runPeripheralPca9685('set');
        }
        assert.equal(calls.length, 1);
    },
    async developer_required() {
        developer = false; handler = async () => response('enabled');
        await app.runPeripheralPca9685('enable'); await app.runPeripheralPca9685('prepare');
        assert.equal(calls.length, 0);
    }
};
(async () => {
    const name = process.argv[2]; assert.equal(typeof tests[name], 'function');
    await tests[name](); process.stdout.write(name + ' PASS\n');
})().catch(error => { console.error(error); process.exitCode = 1; });
