'use strict';
// Actual module methods and state-ui visibility helpers; DOM/network/time are seams.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '../..');
function load(context, file) {
    vm.runInContext(fs.readFileSync(path.join(root, file), 'utf8'), context, {filename: file});
}
function element() {
    const classes = new Set();
    return {value: '', textContent: '', style: {removeProperty(name) {delete this[name];}},
        classList: {add: name => classes.add(name), remove: name => classes.delete(name),
            contains: name => classes.has(name)}};
}
function checkTypes() {
    const nodes = new Map(), groups = [];
    const add = (id, group = false) => {
        const node = element(); nodes.set(id, node); if (group) groups.push(node); return node;
    };
    const app = {registerModule(name, methods) {Object.assign(this, methods);}};
    const context = vm.createContext({AppState: app, window: {}, console, document: {
        getElementById: id => nodes.get(id) || null,
        querySelectorAll: selector => selector === '.peripheral-params-group' ? groups : []
    }});
    load(context, 'web-src/js/state-ui.js');
    load(context, 'web-src/modules/runtime/peripherals.js');
    const expected = [[1, 'uart'], [2, 'i2c'], [3, 'spi'], [15, 'adc'], [26, 'adc'], [27, 'dac'],
        [36, 'lcd'], [37, 'sdcard'], [38, 'generic-sensor'], [41, 'servo'], [42, 'stepper'],
        [43, 'encoder'], [45, 'neopixel'], [47, 'segment'], [48, 'rf'], [49, 'radar'],
        [50, 'ds1302'], [52, 'lcd1602'], [55, 'io-expander'], [56, 'display'], [57, 'pca9685']];
    for (let type = 11; type <= 21; type++) if (type !== 15) expected.push([type, 'gpio']);
    for (const name of new Set(expected.map(row => row[1]))) add(name + '-params', true);
    const gpioChildren = new Map(['.pwm-only', '.ao-only', '.input-only'].map(key => [key, [element()]]));
    nodes.get('gpio-params').querySelectorAll = selector => gpioChildren.get(selector) || [];
    const hints = [[60, 'device-event'], [38, 'generic-sensor'], [50, 'ds1302'], [52, 'lcd1602']];
    hints.forEach(([, name]) => add(name + '-hint'));
    for (const id of ['uart-pin-direction-hint', 'peripheral-pins-group',
        'peripheral-pins-recommend', 'peripheral-original-id']) add(id);
    for (const kind of ['ir', 'rf']) {
        add(kind + '-manage-section', true); add(kind + '-manage-hint-new'); add(kind + '-manage-content');
    }
    let calls = [];
    for (const method of ['_updatePeripheralTypeAvailability', 'teardownIrManage', 'teardownRfManage',
        '_updateGenericSensorDriverFields', '_updateIoExpanderFields', '_updateDisplayFields',
        '_updatePca9685Fields', '_updateSdcardPinHints', '_updateGpioSourceFields',
        '_updatePeripheralPinRoleHint', '_initRfManagePanel', '_initIrManagePanel']) {
        app[method] = (...args) => calls.push([method, ...args]);
    }
    app._getPinRecommendation = type => type === 1 ? 'UART pins' : '';
    const shown = id => !nodes.get(id).classList.contains('is-hidden') && nodes.get(id).style.display !== 'none';
    const updates = {38: '_updateGenericSensorDriverFields', 55: '_updateIoExpanderFields',
        56: '_updateDisplayFields', 57: '_updatePca9685Fields', 37: '_updateSdcardPinHints'};
    for (const [type, group] of [...expected, [53, null], [51, null], [60, null], [999, null]]) {
        calls = []; app.onPeripheralTypeChange(String(type));
        for (const name of new Set(expected.map(row => row[1]))) {
            assert.equal(shown(name + '-params'), name === group, type + ': ' + name);
        }
        assert.equal(shown('rf-manage-section'), type === 48);
        assert.equal(shown('ir-manage-section'), type === 53);
        assert.equal(shown('uart-pin-direction-hint'), type === 1);
        assert.equal(shown('peripheral-pins-group'), ![51, 56, 57, 60].includes(type));
        hints.forEach(([value, name]) => assert.equal(shown(name + '-hint'), type === value));
        assert.equal(nodes.get('peripheral-pins-recommend').textContent, type === 1 ? 'UART pins' : '');
        assert(calls.some(call => call[0] === '_updatePeripheralTypeAvailability' && call[1] === type));
        assert.equal(calls.some(call => call[0] === 'teardownIrManage'), type !== 53);
        assert.equal(calls.some(call => call[0] === 'teardownRfManage'), type !== 48);
        for (const [value, method] of Object.entries(updates)) {
            const found = calls.find(call => call[0] === method);
            assert.equal(!!found, type === Number(value), method);
            if (found && method === '_updateIoExpanderFields') assert.equal(found[1], false);
        }
        assert.equal(calls.filter(call => call[0] === '_updateGpioSourceFields').length, 1);
        assert(calls.some(call => call[0] === '_updatePeripheralPinRoleHint' && call[1] === type));
        if (group === 'gpio') {
            for (const selector of ['.pwm-only', '.ao-only'])
                assert.equal(!gpioChildren.get(selector)[0].classList.contains('is-hidden'), [16, 17].includes(type));
            assert.equal(!gpioChildren.get('.input-only')[0].classList.contains('is-hidden'),
                [11, 13, 14, 18, 19, 20].includes(type));
        }
    }
    for (const [type, kind, method] of [[48, 'rf', '_initRfManagePanel'], [53, 'ir', '_initIrManagePanel']]) {
        for (const originalId of ['', 'saved-id']) {
            calls = []; nodes.get('peripheral-original-id').value = originalId;
            app.onPeripheralTypeChange(type);
            assert.equal(nodes.get(kind + '-manage-hint-new').style.display, originalId ? 'none' : '');
            assert.equal(nodes.get(kind + '-manage-content').style.display, originalId ? '' : 'none');
            assert.equal(calls.filter(call => call[0] === method).length, originalId ? 1 : 0);
        }
        // Optional management markup may be absent in a constrained UI.
        nodes.delete(kind + '-manage-section'); calls = []; app.onPeripheralTypeChange(type);
        assert.equal(calls.filter(call => call[0] === method).length, 0);
    }
    nodes.clear();
    app.onPeripheralTypeChange(57); // Null optional nodes must remain safe.
}
async function checkEvents() {
    const app = {}, calls = [];
    let now = 1000;
    function getter(fresh) {
        return url => new Promise((resolve, reject) => calls.push({url, fresh, resolve, reject}));
    }
    const context = vm.createContext({AppState: app, window: {}, document: {},
        Date: {now: () => now}, apiGet: getter(false), apiGetFresh: getter(true)});
    load(context, 'web-src/modules/runtime/periph-exec-form.js');
    const catalogs = [
        ['_getPeriphExecEventCategories', '_peEventCategories', 'categories', 120000],
        ['_getPeriphExecStaticEvents', '_peStaticEvents', 'static', 120000],
        ['_getPeriphExecDynamicEvents', '_peDynamicEvents', 'dynamic', 15000]
    ];
    for (const [method, key, endpoint, ttl] of catalogs) {
        app._clearPeriphExecEventCatalogCache(); calls.length = 0;
        const pending = app[method](false);
        assert.equal(app[method](false), pending, 'concurrent normal reads share one promise');
        assert.equal(calls.length, 1);
        assert.equal(calls[0].url, '/api/periph-exec/events/' + endpoint); assert.equal(calls[0].fresh, false);
        const source = [{id: endpoint}]; calls[0].resolve({success: true, data: source});
        const cached = await pending;
        assert.notEqual(cached, source, 'response array is copied'); assert.equal(cached[0], source[0]);
        assert.equal(app[key + 'Promise'], null);
        const fetchedAt = app[key + 'FetchedAt'];
        now = fetchedAt + ttl - 1;
        assert.equal(await app[method](false), cached); assert.equal(calls.length, 1);
        now++;
        const expired = app[method](false); assert.equal(calls.length, 2);
        calls[1].reject(new Error('offline'));
        assert.equal(await expired, cached, 'nonempty cached results remain usable on a failed refresh');
        assert.equal(app[key + 'FetchedAt'], fetchedAt, 'fallback must not reset sample age');
        assert.equal(app[key + 'Promise'], null);

        const fresh = app[method](true);
        assert.equal(calls.at(-1).fresh, true); calls.at(-1).resolve({success: true, data: []});
        assert.equal((await fresh).length, 0);
        const failedEmpty = app[method](true); calls.at(-1).reject(new Error('empty-cache'));
        await assert.rejects(failedEmpty, /empty-cache/);
        assert.equal(app[key + 'Promise'], null);
        // A forced request keeps its original bypass behavior even with one normal request pending.
        app._clearPeriphExecEventCatalogCache();
        const normal = app[method](false), normalCall = calls.at(-1);
        const forced = app[method](true), forcedCall = calls.at(-1);
        assert.notEqual(normal, forced); assert.notEqual(normalCall, forcedCall); assert.equal(forcedCall.fresh, true);
        normalCall.resolve({success: true, data: [{id: 'normal'}]}); await normal;
        forcedCall.resolve({success: true, data: [{id: 'forced'}]}); await forced;
        delete context.apiGetFresh;
        const fallback = app[method](true); assert.equal(calls.at(-1).fresh, false);
        calls.at(-1).resolve({success: false, data: [{id: 'rejected-data'}]});
        assert.equal((await fallback).length, 0, 'unsuccessful response data is not cached');
        context.apiGetFresh = getter(true);
    }
    app._clearPeriphExecEventCatalogCache();
    const pending = catalogs.map(([method]) => app[method](false));
    const requests = calls.slice(-3); assert.equal(new Set(requests.map(call => call.url)).size, 3);
    requests.forEach((call, index) => call.resolve({success: true, data: [{id: index}]}));
    const lists = await Promise.all(pending);
    lists.forEach((list, index) => assert.equal(list[0].id, index, 'catalogs must have independent storage'));
    app._clearPeriphExecEventCatalogCache();
    catalogs.forEach(([, key]) => {
        assert.equal(app[key], null); assert.equal(app[key + 'FetchedAt'], 0); assert.equal(app[key + 'Promise'], null);
    });
}
async function checkRefillAndBindings() {
    const nodes = new Map(), calls = [], errors = [];
    const node = id => {
        if (!nodes.has(id)) nodes.set(id, {value: 'default', checked: false,
            addEventListener(event, callback) {calls.push({id, event, callback});}});
        return nodes.get(id);
    };
    let response, resolveResponse;
    const app = {registerModule(name, methods) {Object.assign(this, methods);}};
    const lazy = [];
    const context = vm.createContext({AppState: app, window: {}, console, Notification: {
        error: (...args) => errors.push(args)}, apiGetFresh: () => response,
        ModuleLoader: {loadModule(name, callback) {lazy.push({name, callback});}},
        document: {getElementById: node}});
    load(context, 'web-src/modules/runtime/peripherals.js');
    app._periphFormSession = {};
    for (const method of ['_syncPeripheralTypeSelect', 'onPeripheralTypeChange', '_updateSdcardPinHints'])
        app[method] = () => {};
    const data = {id: 'rf-tx', name: '无线', type: 48, enabled: true, pins: [4, 255], params: {
        initialState: 0, pwmFrequency: 1000, pwmResolution: 12, baudRate: 19200, dataBits: 8,
        stopBits: 2, parity: 1, address: 56, mode: 0, resolution: 10, attenuation: 2,
        brightness: 0, stepsPerRevolution: 2048, speed: 0, count: 16, bitLength: 24,
        pulseWidth: 350, repeat: 1, activeHigh: false, debounceMs: 0, holdMs: 1500,
        width: 128, height: 64, interface: 0, contrast: 127, sampleRate: 0,
        sampleCount: 1, filterMode: 0, sensorType: 0, sampleInterval: 500, busPeripheralId: 'bus-a'}};
    response = Promise.resolve({success: true, data});
    await app.loadPeripheralForEdit(data.id);
    const expected = {'gpio-initial-state': 0, 'gpio-pwm-freq': 1000, 'gpio-pwm-resolution': 12,
        'uart-baudrate': 19200, 'uart-databits': 8, 'uart-stopbits': 2, 'uart-parity': 1,
        'i2c-address': 56, 'spi-mode': 0, 'adc-resolution': 10, 'adc-attenuation': 2,
        'segment-brightness': 0, 'stepper-steps-per-rev': 2048, 'stepper-speed': 0,
        'neopixel-count': 16, 'neopixel-brightness': 0, 'rf-mode': 0, 'rf-bit-length': 24,
        'rf-pulse-width': 350, 'rf-repeat': 1, 'radar-debounce-ms': 0, 'radar-hold-ms': 1500,
        'lcd-width': 128, 'lcd-height': 64, 'lcd-interface': 0, 'lcd-contrast': 127,
        'encoder-resolution': 10, 'adc-sample-rate': '0', 'adc-sample-count': '1',
        'adc-filter-mode': '0', 'generic-sensor-type': '0', 'generic-sensor-interval': '500',
        'generic-sensor-bus-id': 'bus-a'};
    for (const [id, value] of Object.entries(expected)) assert.equal(node(id).value, value, id);
    assert.equal(node('rf-active-high').checked, false);
    assert.equal(node('radar-active-high').checked, false);
    assert.equal(node('peripheral-pins-input').value, '4');
    response = Promise.resolve({success: true, data: {...data, params: {}}});
    await app.loadPeripheralForEdit(data.id);
    for (const [id, value] of Object.entries(expected)) assert.equal(node(id).value, value, 'omitted ' + id);
    response = new Promise(resolve => {resolveResponse = resolve;});
    const stale = app.loadPeripheralForEdit(data.id);
    app._periphFormSession = {};
    resolveResponse({success: true, data: {...data, params: {repeat: 20}}});
    await stale; assert.equal(node('rf-repeat').value, 1);
    app._bindPeripheralModalEvents(); const bound = calls.length; app._bindPeripheralModalEvents();
    assert.equal(calls.length, bound);
    let closed = 0, displayed = 0; app.closePeripheralModal = () => closed++;
    app._updateDisplayFields = () => displayed++;
    for (const id of ['close-peripheral-modal', 'cancel-peripheral-btn', 'close-gpio-modal', 'cancel-gpio-btn'])
        calls.find(item => item.id === id && item.event === 'click').callback();
    for (const id of ['display-touch-backend', 'display-backend', 'display-driver', 'display-backlight'])
        calls.find(item => item.id === id).callback();
    assert.equal(closed, 4); assert.equal(displayed, 4);
    let ir = 0, rf = 0;
    app.initIrManage = () => ir++; app.initRfManage = () => rf++;
    app._initIrManagePanel(); app._initRfManagePanel();
    assert.deepEqual(lazy.map(item => item.name), ['ir-remote', 'rf-remote']);
    lazy.forEach(item => item.callback()); assert.equal(ir, 1); assert.equal(rf, 1);
    delete context.ModuleLoader; app._initIrManagePanel(); app._initRfManagePanel();
    assert.equal(ir, 1); assert.equal(rf, 1); assert.equal(errors.length, 0);
}
Promise.resolve().then(() => {
    if (process.argv[2] === 'types') return checkTypes();
    if (process.argv[2] === 'events') return checkEvents();
    if (process.argv[2] === 'refill') return checkRefillAndBindings();
    throw new Error('expected types or events');
}).catch(error => {console.error(error); process.exitCode = 1;});
