'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

function setup() {
    let now = 100, watch;
    const value = {}, unit = {}, status = {};
    const card = {dataset: {id: 'temperature'}, querySelector: s => ({
        '.dc-monitor-value': value, '.dc-monitor-unit': unit, '.dc-monitor-status': status})[s]};
    const app = {_dcLifecycleGeneration: 1, _dcIsLifecycleActive: () => true,
        _buildMonitorGroupsFromModbus: () => [],
        _getMonitorUnit: () => 'inferred', onSSEEvent() {}, offSSEEvent() {}, connectSSE() {},
        disconnectSSE() {}, _dcScheduleSnapshotRecovery() {}};
    const context = vm.createContext({AppState: app, console, performance: {now: () => now},
        escapeHtml: String, setTimeout: fn => {watch = fn; return 1;}, clearTimeout() {},
        document: {querySelector: s => s.includes('temperature') ? card : null,
            querySelectorAll: () => Object.hasOwn(card.dataset, 'sampleExpiresAt') ? [card] : []}});
    for (const file of ['render.js', 'modbus-ops.js']) vm.runInContext(fs.readFileSync(path.join(
        __dirname, '../../web-src/modules/runtime/device-control', file), 'utf8'), context);
    return {app, card, value, unit, advance: n => {now += n;}, setTime: n => {now = n;}, tick: () => watch()};
}
const fresh = () => ({key: 'temperature', label: '温度', value: '23.5', unit: '°C',
    valid: true, quality: 'good', ageMs: 200, maxAgeMs: 1000});

test('sample age survives rerender and expires without a network event', () => {
    const s = setup(), sample = fresh(), data = {sensor: [{id: 'temperature-rule', sensors: [sample]}]};
    s.app._renderMonitorDataCard(data);
    assert.equal(sample._dcSampleReceivedAt, 100);
    s.advance(799); assert.equal(s.app._dcMonitorValue(sample), '23.5');
    s.app._renderMonitorDataCard(data);
    s.advance(1); assert.equal(s.app._dcMonitorValue(sample), '--');
    assert.equal(s.app._dcMonitorPresentation(sample, sample.key).quality, 'stale');
});
test('duplicate replacement copies sample age and actual empty unit together', () => {
    const s = setup(), captured = [];
    s.app._renderMonitorCard = card => {captured.push(card); return '';};
    const data = {sensor: [{id: 'temperature-rule', sensors: [{...fresh(), valid: false, unit: 'old'}, {...fresh(), unit: ''}]}]};
    s.app._renderMonitorDataCard(data);
    assert.equal(captured.length, 1); assert.equal(captured[0].unit, '');
    assert.equal(captured[0].ageMs, 200); assert.equal(captured[0]._dcSampleReceivedAt, 100);
    s.advance(800); assert.equal(s.app._dcMonitorValue(captured[0]), '--');
});
test('SSE updates cache timestamp and clears obsolete unit; local watch marks expiry', () => {
    const s = setup(), sample = fresh(); s.app._controlData = {sensor: [{id: 'temperature-rule', sensors: [sample]}]};
    s.app._setupSSE(); s.app._handleSSESensorData({...fresh(), unit: ''});
    assert.equal(s.value.textContent, '23.5'); assert.equal(s.unit.textContent, '');
    assert.equal(sample._dcSampleReceivedAt, 100); assert.equal(s.card.dataset.sampleExpiresAt, '900');
    s.advance(800); s.tick();
    assert.equal(s.value.textContent, '--'); assert.equal(s.card.dataset.quality, 'stale');
    assert.equal(s.card.dataset.measurementValid, 'false');
    assert.equal(s.app._dcMonitorValue(sample), '--');
});
test('malformed age, clock reversal and explicitly stale quality never present a measurement', () => {
    const s = setup();
    for (const change of [{ageMs: '0'}, {ageMs: -1}, {maxAgeMs: 0}, {maxAgeMs: NaN},
        {_dcSampleReceivedAt: Infinity}, {_dcSampleReceivedAt: 101}, {quality: 'stale'},
        {value: 'Infinity'}, {valid: false}]) assert.equal(s.app._dcMonitorValue({...fresh(), ...change}), '--');
    assert.equal(s.app._dcMonitorValue({value: '23'}), '23'); // Existing non-sensor cards remain compatible.
    s.setTime(NaN); assert.equal(s.app._dcMonitorValue(fresh()), '--');
});
test('expired soil placeholder stays visibly invalid and new SSE restores validity', () => {
    const s = setup(); s.card.dataset.id = 'ai_soil_humidity';
    s.app._dcUpdateMonitorCard(s.card, {...fresh(), ageMs: 1000});
    assert.equal(s.value.textContent, '0'); assert.equal(s.card.dataset.valuePlaceholder, 'true');
    assert.equal(s.card.dataset.measurementValid, 'false');
    s.app._dcUpdateMonitorCard(s.card, fresh());
    assert.equal(s.card.dataset.valuePlaceholder, 'false'); assert.equal(s.card.dataset.measurementValid, 'true');
});
