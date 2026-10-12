'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

function setup() {
    const app = {registerModule(name, members) { Object.assign(this, members); },
        _hiddenClass: visible => visible ? '' : ' hidden', _renderSensorDataFieldOptions: () => '',
        _syncDhtSensorField() {}};
    vm.runInNewContext(fs.readFileSync(path.join(__dirname,
        '../../web-src/modules/runtime/periph-exec-sensor.js'), 'utf8'), {AppState: app,
        escapeHtml: value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;', '<':'&lt;',
            '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c]))});
    return app;
}
function collect(app, address, index = '0', category = 'ds18b20') {
    const values = {'.pe-sensor-category': category, '.pe-sensor-address': address,
        '.pe-sensor-devindex': index, '.pe-target-periph': 'probe', '.pe-sensor-datafield': 'temperature'};
    const action = {};
    app._collectPeriphExecSensorAction({querySelector: s => s in values ? {value: values[s]} : null}, action);
    return JSON.parse(action.actionValue);
}
test('explicit ROM round trips outside hidden extra JSON and preserves leading zero bytes', () => {
    const app = setup(), config = collect(app, '280000000000001e', '15');
    const state = app._getPeriphExecSensorPanelState({actionValue: JSON.stringify(config)});
    assert.equal(state.config.deviceAddress, '280000000000001e');
    assert.equal(state.config.deviceIndex, 15); assert.equal(state.extraJson, '');
    assert.match(app._buildPeriphExecSensorPanel({actionValue: JSON.stringify(config)}), /280000000000001e/);
});
test('legacy index defaults and unrelated sensors remain separate', () => {
    const app = setup();
    assert.equal(collect(app, '').deviceIndex, 0);
    assert.equal(collect(app, '').deviceAddress, '');
    assert.ok(!('deviceAddress' in collect(app, 'bad', '0', 'dht11')));
});
test('invalid user input blocks save instead of selecting index zero or dropping the ROM', () => {
    for (const index of ['-1','16','1.5','bad']) {
        const app = setup(); collect(app, '', index);
        assert.match(app._peConfiguredSensorError, /索引/);
    }
    for (const address of ['280000000000001e00', '100000000000001e', '28xx', '28000000\0xxxxxxx']) {
        const app = setup(), result = collect(app, address);
        assert.equal(result.deviceAddress, address); assert.match(app._peConfiguredSensorError, /ROM/);
    }
});
test('stored ROM and legacy index cannot inject HTML', () => {
    const app = setup();
    const html = app._buildPeriphExecSensorPanel({actionValue: JSON.stringify({sensorCategory: 'ds18b20',
        deviceAddress: '"><img src=x>', deviceIndex: '"><script>'})});
    assert.ok(!html.includes('<img') && !html.includes('<script>'));
    assert.match(html, /&lt;img/);
});
