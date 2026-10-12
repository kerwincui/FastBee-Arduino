'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

function setup() {
    const app = {registerModule(name, members) { Object.assign(this, members); }};
    const document = {getElementById: () => null};
    const context = {AppState: app, document, window: {}, Notification: {},
        escapeHtml: value => String(value).replace(/[&<>"']/g, character => ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[character]))};
    vm.createContext(context);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/js/utils.js'), 'utf8'), context);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/periph-exec.js'), 'utf8'), context);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/periph-exec-form.js'), 'utf8'), context);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/periph-exec-templates.js'), 'utf8'), context);
    vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/periph-exec-sensor.js'), 'utf8'), context);
    app._pePeripherals = [{id: 'relay', type: 12, enabled: true, pins: [26],
        params: {initialState: 1, invertLogic: false, passive: false, ioSource: 'native'}},
        {id: 'room', type: 38, enabled: true, pins: [21, 22], params: {}}];
    app._peSensorSources = [{id: 'room_temperature', periphId: 'room', unit: '°C', label: '室温'}];
    // Produced by the actual C++ catalog writer in the Python test entrypoint.
    app._peTypeCatalog = [{value: 12, thermostatConfig: JSON.parse(process.env.FASTBEE_TEST_THERMOSTAT_SCHEMA)}];
    const config = {sourceRef: 'room_temperature', mode: 'heat', setpoint: 25, hysteresis: 1,
        minOnSec: 30, minOffSec: 30, maxAgeSec: 30, recovery: 'manual', startup: 'fresh', fault: 'off'};
    const rule = {triggers: [{triggerType: 1, timerMode: 0, intervalSec: 1}],
        actions: [{actionType: 39, targetPeriphId: 'relay', syncDelayMs: 0}], allowPanelControl: false, recordHistory: false};
    return {app, config, rule, context, document};
}

test('DHT channel changes and collection use physical units without relabelling custom names', () => {
    const {app} = setup();
    const elements = new Map([
        ['.pe-sensor-category', {value: 'dht11'}],
        ['.pe-sensor-datafield', {value: 'humidity', classList: {contains: name => name === 'pe-sensor-datafield'}}],
        ['.pe-sensor-unit', {value: '℃', readOnly: false}],
        ['.pe-sensor-label', {value: '温度'}],
        ['.pe-target-periph', {value: 'room'}]
    ]);
    const block = {querySelector: selector => elements.get(selector)};
    app._getPeriphExecBlockIndex = () => 0;
    app._getPeriphExecBlock = () => block;
    app._refreshPeriphExecRiskNotice = () => {};
    // Exercise the actual delegated change entry, not just the new helper.
    app._handlePeriphExecActionChange({target: elements.get('.pe-sensor-datafield')});
    assert.equal(elements.get('.pe-sensor-unit').value, '%');
    assert.equal(elements.get('.pe-sensor-unit').readOnly, true);
    assert.equal(elements.get('.pe-sensor-label').value, '湿度');
    elements.get('.pe-sensor-unit').value = '℃'; // stale/imported form payload
    const action = {};
    app._collectPeriphExecSensorAction(block, action);
    assert.equal(JSON.parse(action.actionValue).unit, '%');
    assert.equal(JSON.parse(action.actionValue).dataField, 'humidity');
    elements.get('.pe-sensor-category').value = 'DHT22';
    elements.get('.pe-sensor-datafield').value = 'temperature';
    elements.get('.pe-sensor-label').value = '自定义机房读数';
    app._handlePeriphExecActionChange({target: elements.get('.pe-sensor-datafield')});
    assert.equal(elements.get('.pe-sensor-unit').value, '°C');
    assert.equal(elements.get('.pe-sensor-label').value, '自定义机房读数');
    elements.get('.pe-sensor-category').value = 'analog';
    elements.get('.pe-sensor-unit').value = '℃';
    app._syncDhtSensorField(block);
    assert.equal(elements.get('.pe-sensor-unit').value, '℃'); // calibrated analogue units remain literal
});

test('only enabled native inactive-high plain digital outputs are eligible', () => {
    const {app} = setup();
    const original = app._pePeripherals[0];
    const variants = [{type: 17}, {enabled: false}, {pins: [26, 27]}, {pins: [255]},
        {params: {...original.params, initialState: 0}}, {params: {...original.params, invertLogic: true}},
        {params: {...original.params, passive: true}}, {params: {...original.params, ioSource: 'expander'}}];
    variants.forEach((patch, index) => app._pePeripherals.push({...original, ...patch, id: 'wrong' + index}));
    assert.equal(JSON.stringify(app._thermostatTargets().map(p => p.id)), '["relay"]');
});

test('sources require saved Celsius channels and an enabled configured peripheral', () => {
    const {app} = setup();
    app._peSensorSources.push({id: 'humidity', periphId: 'room', unit: '%'},
        {id: 'missing', periphId: 'unknown', unit: '°C'}, {id: 'raw_uart', periphId: 'room'});
    assert.equal(JSON.stringify(app._thermostatSources().map(s => s.id)), '["room_temperature"]');
    app._pePeripherals[1].enabled = false;
    assert.equal(app._thermostatSources().length, 0);
});

test('roundtrip form values preserve custom dwell, threshold, and safety policy', () => {
    const {app, config, rule} = setup();
    Object.assign(config, {mode: 'cool', setpoint: 30.5, hysteresis: 2.5, minOnSec: 75, minOffSec: 900, recovery: 'fresh'});
    const block = {querySelector: selector => ({value: String(config[selector.replace('.pe-thermostat-', '')])})};
    const result = app._collectThermostatConfig(block);
    assert.equal(JSON.stringify(result), JSON.stringify({sourceRef: config.sourceRef, mode: config.mode,
        recovery: config.recovery, startup: config.startup, fault: config.fault, setpoint: config.setpoint,
        hysteresis: config.hysteresis, minOnSec: config.minOnSec, minOffSec: config.minOffSec, maxAgeSec: config.maxAgeSec}));
    assert.equal(app._validateThermostatForm(rule, result), '');
});

test('blank, fractional, out-of-range and unsafe values cannot pass save review', () => {
    const {app, config, rule} = setup();
    for (const patch of [{setpoint: NaN}, {hysteresis: 0}, {setpoint: 125}, {minOffSec: 0},
        {minOnSec: 1.5}, {maxAgeSec: 3601}, {fault: 'on'}, {startup: 'on'}, {recovery: 'hold'}, {sourceRef: 'unknown'}]) {
        assert.notEqual(app._validateThermostatForm(rule, {...config, ...patch}), '');
    }
    const empty = app._collectThermostatConfig({querySelector: () => ({value: ''})});
    assert.ok(Number.isNaN(empty.setpoint));
});

test('mixed actions, arbitrary triggers, delay, history and panel control are rejected', () => {
    const {app, config, rule} = setup();
    for (const patch of [{actions: [...rule.actions, rule.actions[0]]}, {allowPanelControl: true},
        {recordHistory: true}, {triggers: [{triggerType: 0}]},
        {actions: [{...rule.actions[0], syncDelayMs: 1}]}])
        assert.notEqual(app._validateThermostatForm({...rule, ...patch}, config), '');
});

test('saved unknown source remains visible as invalid instead of silently selecting another', () => {
    const {app, config} = setup();
    config.sourceRef = '<unknown>';
    const markup = app._renderThermostatPanel(JSON.stringify(config));
    assert.ok(markup.includes('selected disabled value="&lt;unknown&gt;"'));
    assert.ok(markup.includes('pe-thermostat-minOnSec'));
    assert.ok(markup.includes('pe-thermostat-minOffSec'));
    assert.ok(markup.includes('pe-thermostat-startup'));
    assert.ok(markup.includes('pe-thermostat-fault'));
    assert.ok(markup.includes('pe-thermostat-recovery'));
});

test('setup only rewrites the unsaved dedicated form and keeps it disabled', () => {
    const {app, document} = setup();
    const controls = new Map(['enabled', 'report', 'record-history', 'panel-control'].map(key =>
        ['periph-exec-' + key, {checked: true}]));
    const delay = {value: '100'};
    const block = {querySelector: selector => selector === '.pe-action-type' ? {value: '39'} : delay};
    let cleared = 0, trigger;
    controls.set('periph-exec-triggers', {replaceChildren: () => {cleared++;}});
    document.querySelectorAll = () => [block];
    document.getElementById = name => controls.get(name);
    app._createPeriphExecTriggerElement = value => {trigger = value;};
    app._setupThermostatRuleForm();
    assert.equal(cleared, 1);
    assert.equal(trigger.triggerType, 1); assert.equal(trigger.intervalSec, 1);
    assert.equal(delay.value, '0');
    for (const key of ['enabled', 'report', 'record-history', 'panel-control']) assert.equal(controls.get('periph-exec-' + key).checked, false);
});

test('diagnostics never label uncertain or old GPIO feedback as a confirmed safe output', () => {
    const {app, rule} = setup(); rule.id = 'temp';
    app._peThermostats = null;
    assert.match(app._renderThermostatRuntime(rule), /输出状态未知/);
    app._peThermostats = [{ruleId: 'temp', targetId: 'relay', phase: 'RETIRING_OUTPUT_UNCONFIRMED',
        padConfirmed: false, outputOn: true, checkAgeMs: 0, retiring: true}];
    assert.match(app._renderThermostatRuntime(rule), /关闭尚未确认/);
    assert.match(app._renderThermostatRuntime(rule), /引脚状态未确认/);
    assert.match(app._formatPeriphExecCapacitySummary(1), /温控输出待确认：relay/);
    app._peThermostats[0] = {ruleId: 'temp', phase: 'OFF_CONFIRMED', padConfirmed: true, outputOn: false, checkAgeMs: 3000};
    assert.match(app._renderThermostatRuntime(rule), /引脚状态未确认/);
    app._peThermostats[0].checkAgeMs = 0;
    assert.match(app._renderThermostatRuntime(rule), /引脚HIGH/);
    assert.match(app._renderThermostatRuntime(rule), /非负载反馈/);
});

test('actual catalog defaults, limits and enum options drive rendering and validation', () => {
    const {app, config, rule} = setup();
    const schema = app._thermostatConfigSchema();
    assert.ok(schema); assert.equal(Object.keys(schema).length, 10);
    assert.equal(schema.minOffSec.default, config.minOffSec);
    assert.match(app._renderThermostatPanel(''), /pe-thermostat-minOffSec[^>]*min="1"[^>]*max="86400"[^>]*value="30"/);
    // A changed server descriptor must alter the real form; there is no
    // fallback range/default or duplicate acceptance table in JavaScript.
    schema.minOffSec.default = 120; schema.minOffSec.min = 60; schema.minOffSec.max = 600;
    assert.match(app._renderThermostatPanel(''), /pe-thermostat-minOffSec[^>]*min="60"[^>]*max="600"[^>]*value="120"/);
    assert.notEqual(app._validateThermostatForm(rule, config), '');
    assert.equal(app._validateThermostatForm(rule, {...config, minOffSec: 120}), '');
    schema.recovery.choices = ['manual'];
    assert.notEqual(app._validateThermostatForm(rule, {...config, minOffSec: 120, recovery: 'fresh'}), '');
    assert.match(app._renderThermostatPanel(JSON.stringify({...config, recovery: 'fresh'})), /selected disabled value="fresh"/);
});

test('missing, duplicate, future or malformed metadata cannot manufacture an editable thermostat', () => {
    for (const mutate of [
        app => {app._peTypeCatalog = [];},
        app => {app._peTypeCatalog[0].thermostatConfig.version = 99;},
        app => {app._peTypeCatalog[0].thermostatConfig.fields[1].max = '125';},
        app => {app._peTypeCatalog[0].thermostatConfig.fields[1] = app._peTypeCatalog[0].thermostatConfig.fields[0];}
    ]) {
        const {app, config, rule} = setup(); mutate(app);
        assert.equal(app._thermostatConfigSchema(), null);
        assert.match(app._validateThermostatForm(rule, config), /未提供完整/);
        assert.match(app._renderThermostatPanel(JSON.stringify(config)), /已保存配置保留/);
        assert.doesNotMatch(app._renderThermostatPanel(JSON.stringify(config)), /<input|<select/);
    }
});
