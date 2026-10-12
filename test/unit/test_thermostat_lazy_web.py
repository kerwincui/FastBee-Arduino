"""Real editor DOM and deferred sensor-module loading; no API/device access."""
from pathlib import Path
import os
import subprocess
import unittest
from thermostat_schema_fixture import thermostat_schema_json


ROOT = Path(__file__).resolve().parents[2]
PROBE = r"""
const assert = require('node:assert/strict');
const fs = require('node:fs');
const {chromium} = require('./test/browser/node_modules/playwright');
(async () => {
  const browser = await chromium.launch({headless: true});
  try {
    const page = await browser.newPage();
    await page.setContent('<div id="periph-exec-actions"></div><div id="periph-exec-triggers"></div>' +
      '<input id="periph-exec-original-id" value="temp"><input id="periph-exec-name" value="Room">' +
      '<input type="checkbox" id="periph-exec-enabled"><input type="checkbox" id="periph-exec-report">' +
      '<div id="periph-exec-error"></div>');
    await page.evaluate(() => {
      window.escapeHtml = value => String(value ?? '').replace(/[&<>"']/g,
        c => ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c]));
      window.t = (_, fallback) => fallback;
      window.AppState = {registerModule(name, members) {Object.assign(this, members);}};
      window.callbacks = [];
      window.ModuleLoader = {loadModule(name, callback) {callbacks.push({name, callback});}};
      window.fetch = () => {throw new Error('offline editor must not call any API');};
    });
    // Production _setSectionVisible uses AppState.showElement/hideElement.
    // Load those real helpers so missing dependencies fail before editor checks.
    await page.addScriptTag({content: fs.readFileSync('web-src/js/state-ui.js', 'utf8')});
    for (const name of ['periph-exec', 'periph-exec-modbus', 'periph-exec-form', 'periph-exec-templates']) {
      await page.addScriptTag({content: fs.readFileSync('web-src/modules/runtime/' + name + '.js', 'utf8')});
      if (name === 'periph-exec-form') assert.equal(await page.evaluate(() => AppState._isPeriphExecEditorReady()), false);
    }
    assert.equal(await page.evaluate(() => AppState._isPeriphExecEditorReady()), true);
    assert.equal(await page.evaluate(() => ['_pca9685CommandCatalog', '_pca9685CommandError',
      '_syncPca9685ActionFields'].every(name => typeof AppState[name] === 'function')), true);
    await page.evaluate(schema => {
      Object.assign(AppState, {_peEditorDataReady: true, guardDeveloperModeAction: () => true});
      AppState._peTypeCatalog = [{value: 12, thermostatConfig: schema}];
      AppState._pePeripherals = [{id: 'relay', name: 'Relay', type: 12, enabled: true, pins: [26],
        params: {ioSource: 'native', initialState: 1, invertLogic: false, passive: false}},
        {id: 'room', type: 38, enabled: true, pins: [21, 22], params: {}}];
      AppState._peSensorSources = [{id: 'room_temperature', periphId: 'room', unit: '°C', label: 'Room'}];
      window.config = {sourceRef: 'room_temperature', mode: 'cool', setpoint: 30.5, hysteresis: 2.5,
        minOnSec: 75, minOffSec: 900, maxAgeSec: 45, recovery: 'fresh', startup: 'fresh', fault: 'off'};
      window.saved = {actionType: 39, targetPeriphId: 'relay', actionValue: JSON.stringify(config), syncDelayMs: 0};
      AppState._createPeriphExecActionElement(saved, 0);
    }, JSON.parse(process.env.FASTBEE_TEST_THERMOSTAT_SCHEMA));
    const pending = await page.evaluate(() => {
      const action = AppState._collectPeriphExecActions()[0];
      AppState.savePeriphExecRule();
      const before = {action, error: AppState._peLazyModuleError,
        inlineError: document.getElementById('periph-exec-error').textContent,
        errorVisible: !document.getElementById('periph-exec-error').classList.contains('is-hidden'),
        loads: callbacks.map(item => item.name)};
      window.oldBlock = document.querySelector('.periph-exec-config-item');
      AppState._rebuildActionBlocksForTriggerChange();
      before.rebuilt = AppState._collectPeriphExecActions()[0];
      callbacks[0].callback(); // A callback for the removed block cannot render or load again.
      before.detachedEmpty = !oldBlock.querySelector('.pe-thermostat-sourceRef');
      before.loadCount = callbacks.length;
      return before;
    });
    assert.equal(pending.action.actionType, 39);
    assert.equal(pending.action.useReceivedValue, false);
    assert.equal(pending.error, 'periph-exec-sensor');
    assert.match(pending.inlineError, /加载/);
    assert.equal(pending.errorVisible, true);
    assert.deepEqual(pending.loads, ['periph-exec-sensor']);
    assert.equal(pending.rebuilt.actionValue, pending.action.actionValue);
    assert.equal(pending.detachedEmpty, true);
    assert.equal(pending.loadCount, 2);

    // Module callback without a registration is a load failure, not recursion.
    const failed = await page.evaluate(() => {
      callbacks[1].callback();
      const action = AppState._collectPeriphExecActions()[0];
      return {loads: callbacks.length, action, error: AppState._peLazyModuleError,
        message: document.querySelector('.pe-action-lazy-host').textContent};
    });
    assert.equal(failed.loads, 2);
    assert.equal(failed.error, 'periph-exec-sensor');
    assert.equal(failed.action.actionValue, pending.action.actionValue);
    assert.match(failed.message, /加载失败/);

    // Switch away while loading, then arrive late. The current action must stay intact.
    await page.evaluate(() => {
      AppState._rebuildActionBlocksForTriggerChange();
      const select = document.querySelector('.pe-action-type');
      select.value = '0'; AppState.onPeriphExecActionTypeChangeInBlock('0', 0);
    });
    await page.addScriptTag({content: fs.readFileSync('web-src/modules/runtime/periph-exec-sensor.js', 'utf8')});
    const switched = await page.evaluate(() => {
      callbacks[2].callback();
      const before = {type: document.querySelector('.pe-action-type').value,
        thermostat: !!document.querySelector('.pe-thermostat-sourceRef')};
      const select = document.querySelector('.pe-action-type');
      select.value = '39'; AppState.onPeriphExecActionTypeChangeInBlock('39', 0);
      before.restored = AppState._collectPeriphExecActions()[0];
      before.error = AppState._peLazyModuleError;
      return before;
    });
    assert.equal(switched.type, '0'); assert.equal(switched.thermostat, false);
    assert.deepEqual(JSON.parse(switched.restored.actionValue), JSON.parse(pending.action.actionValue));
    assert.equal(switched.error, '');

    const draft = await page.evaluate(() => {
      document.querySelector('.pe-thermostat-setpoint').value = '28.25';
      document.querySelector('.pe-thermostat-minOffSec').value = '1200';
      const select = document.querySelector('.pe-action-type');
      select.value = '0'; AppState.onPeriphExecActionTypeChangeInBlock('0', 0);
      select.value = '39'; AppState.onPeriphExecActionTypeChangeInBlock('39', 0);
      const switched = AppState._collectPeriphExecActions()[0];
      AppState._rebuildActionBlocksForTriggerChange();
      return {switched, rebuilt: AppState._collectPeriphExecActions()[0],
        targets: Array.from(document.querySelector('.pe-target-periph').options).filter(o => o.value).map(o => o.value)};
    });
    for (const action of [draft.switched, draft.rebuilt]) {
      const config = JSON.parse(action.actionValue);
      assert.equal(config.setpoint, 28.25); assert.equal(config.minOffSec, 1200);
      assert.equal(config.sourceRef, 'room_temperature'); assert.equal(config.recovery, 'fresh');
      assert.equal(action.targetPeriphId, 'relay'); assert.equal(action.useReceivedValue, false);
    }
    assert.deepEqual(draft.targets, ['relay']);
    console.log('Thermostat real DOM: pending/failure/rebuild/late callback/draft/target checks passed');
  } finally {await browser.close();}
})().catch(error => {console.error(error); process.exitCode = 1;});
"""


class ThermostatLazyWebTests(unittest.TestCase):
    def test_deferred_module_keeps_saved_values_and_blocks_incomplete_actions(self):
        result = subprocess.run(['node', '-e', PROBE], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', timeout=60,
                                env={**os.environ, 'FASTBEE_TEST_THERMOSTAT_SCHEMA': thermostat_schema_json()})
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
