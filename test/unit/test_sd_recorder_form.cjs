'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const schema = JSON.parse(process.env.FASTBEE_TEST_SD_RECORDER_SCHEMA);
const clone = value => JSON.parse(JSON.stringify(value));
const escapeHtml = value => String(value ?? '').replace(/[&<>"']/g,
    c => ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c]));
function setup() {
    const app = {registerModule(_name, methods) {Object.assign(this, methods);}};
    const context = vm.createContext({AppState: app, window: {}, TextEncoder, TextDecoder,
        document: {getElementById: () => null}, escapeHtml, console});
    for (const name of ['periph-exec', 'periph-exec-templates', 'periph-exec-sensor', 'device-config-transfer'])
        vm.runInContext(fs.readFileSync(path.join(__dirname, '../../web-src/modules/runtime/' + name + '.js'), 'utf8'), context);
    app._peEditorDataReady = true;
    app._peTypeCatalog = [{value: 37, sdRecorderConfig: clone(schema)}];
    app._pePeripherals = [{id: 'sd', type: 37, enabled: true}, {id: 'temp_room', type: 38, enabled: true}];
    app._peSensorSources = [{id: 'temp_room_temperature', periphId: 'temp_room', field: 'temperature', label: 'Room'}];
    const config = {sourceRef: 'temp_room_temperature'};
    for (const field of schema.numbers) config[field.key] = field.defaultValue;
    const rule = {id: 'record', enabled: true, recordHistory: false, allowPanelControl: false, scriptContent: '',
        triggers: [{triggerType: 1, timerMode: 0, intervalSec: 1}],
        actions: [{actionType: 40, targetPeriphId: 'sd', useReceivedValue: false, syncDelayMs: 0, actionValue: JSON.stringify(config)}]};
    return {app, config, rule};
}

test('firmware descriptor drives defaults, limits and disabled template', () => {
    const {app, config, rule} = setup();
    assert.equal(app._validateSdRecorderForm(rule, config), '');
    const draft = app._buildPeriphExecTemplateDraft({kind: 'sd-record', target: 'sd', channel: config.sourceRef, interval: '1'});
    assert.equal(draft.enabled, false); assert.equal(draft.reportAfterExec, false);
    assert.equal(draft.recordHistory, false); assert.equal(draft.allowPanelControl, false);
    assert.equal(draft.triggers[0].intervalSec, 1); assert.equal(draft.actions[0].actionType, 40);
    assert.deepEqual(JSON.parse(draft.actions[0].actionValue), config);
    assert.equal(draft.actions[0].syncDelayMs, 0); assert.equal(draft.actions[0].useReceivedValue, false);
    for (const field of schema.numbers) {
        for (const value of [field.min, field.max]) assert.equal(app._validateSdRecorderForm(rule, {...config, [field.key]: value}), '');
        for (const value of [field.min - 1, field.max + 1, field.min + .5, null, NaN, Infinity, String(field.min)])
            assert.notEqual(app._validateSdRecorderForm(rule, {...config, [field.key]: value}), '');
    }
    assert.notEqual(app._validateSdRecorderForm(rule, {...config, extra: true}), '');
    for (const sourceRef of ['', 'bad source', 'x'.repeat(schema.sourceMaxBytes + 1), 'a\0b', '温度'])
        assert.notEqual(app._validateSdRecorderForm(rule, {...config, sourceRef}), '');
    app._peTypeCatalog[0].sdRecorderConfig.numbers.pop();
    assert.equal(app._sdRecorderConfigSchema(), null);
    assert.match(app._renderSdRecorderPanel(JSON.stringify(config)), /原配置保留/);
    assert.throws(() => app._buildPeriphExecTemplateDraft({kind: 'sd-record', target: 'sd', channel: config.sourceRef}), /完整/);
});

test('saved producer catalog is independent from instantaneous cache; strict rule admission', () => {
    const {app, config, rule} = setup();
    assert.equal(app._validateSdRecorderForm(rule, config), ''); // No cache supplied.
    for (const mutate of [r => r.actions.push(clone(r.actions[0])), r => r.triggers[0].intervalSec = 5,
        r => r.triggers[0].timerMode = 1, r => r.recordHistory = true, r => r.allowPanelControl = true,
        r => r.scriptContent = 'LOG x', r => r.actions[0].useReceivedValue = true, r => r.actions[0].syncDelayMs = 1]) {
        const invalid = clone(rule); mutate(invalid); assert.notEqual(app._validateSdRecorderForm(invalid, config), '');
    }
    app._pePeripherals[0].enabled = false;
    assert.notEqual(app._validateSdRecorderForm(rule, config), '');
    app._pePeripherals[0].enabled = true; app._peSensorSources = [];
    assert.notEqual(app._validateSdRecorderForm(rule, config), '');
    rule.enabled = false; assert.equal(app._validateSdRecorderForm(rule, config), '');
    const saved = app._renderSdRecorderPanel(JSON.stringify(config));
    assert.match(saved, /原来源待核验/); assert.match(saved, /temp_room_temperature/);
    const malicious = app._renderSdRecorderPanel(JSON.stringify({...config, sourceRef: '<img src=x onerror=alert(1)>'}));
    assert.ok(!malicious.includes('<img')); assert.match(malicious, /&lt;img/);
    assert.doesNotThrow(() => app._renderSdRecorderPanel('null'));
});

test('status separates unavailable/inactive/fresh/stale/fault and escapes error content', () => {
    const {app, rule} = setup();
    assert.match(app._renderSdRecorderRuntime(rule), /诊断不可用/);
    app._peSdRecorder = null; assert.match(app._renderSdRecorderRuntime(rule), /无活动/);
    app._peSdRecorder = {ruleId: rule.id, phase: 'RECORDING', checkAgeMs: 0, pending: 2, accepted: 4,
        written: 1, dropped: 1, lastError: '', writeQuality: 'sdk_write_flush_only'};
    assert.match(app._renderSdRecorderRuntime(rule), /记录中.*排队 2\/8.*已写 1.*丢弃 1/);
    app._peSdRecorder.checkAgeMs = 2001; assert.match(app._renderSdRecorderRuntime(rule), /失鲜/);
    app._peSdRecorder.phase = 'SD_RECORD_SOURCE_CHANGED'; app._peSdRecorder.lastError = '<script>alert(1)</script>';
    assert.match(app._renderSdRecorderRuntime(rule), /SD_RECORD_SOURCE_CHANGED/);
    assert.ok(!app._renderSdRecorderRuntime(rule).includes('<script>'));
    assert.equal(app._renderSdRecorderRuntime({actions: [{actionType: 38}]}), '');
    assert.equal(app._renderSdRecorderRuntime({actions: [{actionType: 39}]}), '');
});

test('import detects explicit source ambiguity/missing, requires disabled recorder, keeps implicit unresolved', () => {
    const {app, rule, config} = setup();
    const producer = {enabled: true, recordHistory: true, actions: [{actionType: 19, targetPeriphId: 'temp_room',
        actionValue: JSON.stringify({periphId: 'temp_room', sensorCategory: 'dht11', dataField: 'temperature'})}]};
    rule.enabled = false;
    const preview = rules => app._buildConfigTransferDependencyPreview([
        {name: 'peripherals.json', content: JSON.stringify({peripherals: app._pePeripherals})},
        {name: 'periph_exec.json', content: JSON.stringify({rules})}]);
    assert.equal(preview([producer, rule]).errors.length, 0);
    assert.match(preview([rule]).errors.join(' '), /来源.*缺失/);
    assert.match(preview([producer, clone(producer), rule]).errors.join(' '), /歧义/);
    assert.match(preview([producer, {...rule, enabled: true}]).errors.join(' '), /禁用/);
    producer.actions[0].actionValue = JSON.stringify({periphId: 'temp_room', sensorCategory: 'dht11'});
    const result = preview([producer, rule]);
    assert.equal(result.errors.length, 0); assert.match(result.warnings.join(' '), /隐式通道/);
    assert.equal(app._matchesConfigTransferActionTarget(40, {type: 37}), true);
    assert.equal(app._matchesConfigTransferActionTarget(40, {type: 38}), false);
    assert.equal(app._matchesConfigTransferActionTarget(38, {type: 37}), true);
    assert.equal(config.sourceRef, 'temp_room_temperature');
});
