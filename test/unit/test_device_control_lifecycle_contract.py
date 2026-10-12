import re
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")
PAGE_LOADER = (ROOT / "web-src/js/page-loader.js").read_text(encoding="utf-8")
CORE = (ROOT / "web-src/modules/runtime/device-control/core.js").read_text(encoding="utf-8")
MODBUS_OPS = (ROOT / "web-src/modules/runtime/device-control/modbus-ops.js").read_text(encoding="utf-8")
MODBUS_CTRL = (ROOT / "web-src/modules/runtime/device-control/modbus-ctrl.js").read_text(encoding="utf-8")


class DeviceControlLifecycleContractTests(unittest.TestCase):
    def test_modbus_polling_warning_uses_chinese_and_escapes_other_text(self):
        script = r"""
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {minifyRuntimeJS} = require('./scripts/minify-js');
const raw = fs.readFileSync('web-src/modules/runtime/device-control/render.js', 'utf8');
const route = fs.readFileSync('src/network/handlers/ModbusRouteHandler.cpp', 'utf8');
const messages = Array.from(route.matchAll(/warnings\.add\("([^"]+)"\)/g), row => row[1]);
assert.equal(messages.length, 8);
const known = 'Many polling tasks are enabled; watch bus load and Web response time.';
assert(messages.includes(known));
for (const source of [raw, minifyRuntimeJS(raw)]) {
    const app = {};
    const context = vm.createContext({AppState: app, window: {}});
    context.window = context;
    vm.runInContext(fs.readFileSync('web-src/js/utils.js', 'utf8'), context);
    vm.runInContext(source, context);
    app._modbusStatus = {health: {riskLevel: 'medium', enabledTaskCount: 5, warnings: [known]}};
    const knownHtml = app._renderModbusHealthBanner();
    assert.equal(app._formatDcModbusWarning(known), '轮询任务较多，请留意总线负载和页面响应速度。');
    assert(!knownHtml.includes(known), 'Observed API warning must use the page language');
    assert(knownHtml.includes('轮询任务较多，请留意总线负载和页面响应速度。'));
    assert(knownHtml.includes('Modbus 轮询健康'));
    assert.equal(app._modbusStatus.health.warnings[0], known, 'API data must remain unchanged');
    for (const unknown of [...messages.filter(message => message !== known), 'Unrecognized warning', '<img src=x onerror="alert(1)">', '__proto__']) {
        app._modbusStatus = {health: {riskLevel: 'medium', warnings: [unknown]}};
        const html = app._renderModbusHealthBanner();
        assert.equal(app._formatDcModbusWarning(unknown), unknown);
        assert.equal(app._modbusStatus.health.warnings[0], unknown, 'API data must remain unchanged');
        assert(html.includes(context.escapeHtml(unknown)), 'Unknown messages must remain visible and escaped');
        assert(!html.includes('<img src=x'), 'Unknown warning cannot inject HTML');
    }
    app._modbusStatus = {health: {riskLevel: 'medium', warnings: messages}};
    assert.equal((app._renderModbusHealthBanner().match(/class="dc-risk-warning"/g) || []).length, 3);
    app._modbusStatus = {health: {riskLevel: 'low', enabledTaskCount: 0, warnings: []}};
    assert.equal(app._renderModbusHealthBanner(), '');
}
"""
        result = subprocess.run(["node", "-e", script], cwd=ROOT, capture_output=True,
                                text=True, encoding="utf-8", timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_disabled_direct_page_stops_before_runtime_loader(self):
        gate = STATE.index("const featureUnavailable = !!featureEntry")
        fallback = STATE.index("disabledError.code = 'FEATURE_DISABLED'", gate)
        module_load = STATE.index("this._loadModule(moduleName, runLoader)", fallback)
        self.assertLess(gate, fallback)
        self.assertLess(fallback, module_load)
        disabled_block = re.search(
            r"if\s*\(\s*featureUnavailable\s*\)\s*\{(?P<body>.*?)\}",
            STATE[fallback:module_load], re.S,
        )
        self.assertIsNotNone(disabled_block)
        self.assertRegex(disabled_block.group("body"), r"\breturn\s*;")
        self.assertNotIn("runLoader()", disabled_block.group("body"))
        self.assertIn("data-page-fallback', isFeatureDisabled ? 'feature-disabled'", PAGE_LOADER)

    def test_page_hide_releases_all_device_control_state(self):
        self.assertIn("AppState.onPageHide('device-control'", CORE)
        self.assertIn("this.destroyDeviceControlPage({ abortRequests: false })", CORE)
        destroy_start = CORE.index("destroyDeviceControlPage: function(options)")
        destroy_end = CORE.index("_ensureDeviceControlRuntime: function()", destroy_start)
        destroy = CORE[destroy_start:destroy_end]
        for cleanup in (
            "this._dcLifecycleGeneration++",
            "this._dcCancelPendingDelays()",
            "this._dcStopAllAutoRefresh()",
            "this._dcCancelLayoutInteraction()",
            "this._dcPageLoadPromise = null",
            "this._dcClearRuntimeState()",
        ):
            self.assertIn(cleanup, destroy)

        clear_start = CORE.index("_dcClearRuntimeState: function()")
        clear_end = CORE.index("_dcCancelPendingDelays: function()", clear_start)
        clear = CORE[clear_start:clear_end]
        for state in (
            "_controlData = null",
            "_modbusStatus = null",
            "_modbusConfiguredDevices = []",
            "_modbusDevices = []",
            "_dcCoilStates = {}",
            "_dcPwmStates = {}",
            "_dcPidValues = {}",
            "_dcDeviceOnline = {}",
        ):
            self.assertIn(state, clear)

    def test_configured_modbus_inventory_is_separate_from_active_polling_devices(self):
        self.assertIn("_modbusConfiguredDevices: []", CORE)
        self.assertIn("this._modbusConfiguredDevices = rtu.master.devices.map", CORE)
        self.assertIn("this._modbusDevices = this._modbusConfiguredDevices.filter", CORE)
        self.assertIn("return device.enabled !== false", CORE)

    def test_initial_page_load_does_not_call_motor_command_endpoint(self):
        init_start = CORE.index("_dcInitModbusDeviceStates: async function(")
        init_end = CORE.index("_dcStopAllAutoRefresh: function(", init_start)
        init_body = CORE[init_start:init_end]
        self.assertNotIn("_dcRefreshMotorStatus", init_body)
        self.assertIn("explicit refresh button", init_body)
        self.assertIn("_dcRefreshMotorStatus: function(devIdx)", MODBUS_CTRL)

    def test_initial_motor_state_loading_is_read_only_for_both_modes(self):
        script = """
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const {minifyRuntimeJS} = require('./scripts/minify-js');
const raw = fs.readFileSync('web-src/modules/runtime/device-control/core.js', 'utf8');
(async () => {
  for (const source of [raw, minifyRuntimeJS(raw)]) {
    for (const immediate of [false, true]) {
      const state = {}, calls = [];
      const context = vm.createContext({AppState: state, window: {}});
      vm.runInContext(fs.readFileSync('web-src/js/utils.js', 'utf8'), context);
      vm.runInContext(source, context);
      Object.assign(state, {
        _modbusDevices: ['motor', 'relay', 'pwm', 'pid', 'motor'].map(deviceType => ({deviceType})),
        _dcIsLifecycleActive: () => true,
        _dcDelay: async () => true,
        _dcRefreshCoilStatus: async () => calls.push('relay'),
        _dcRefreshPwmStatus: async () => calls.push('pwm'),
        _dcRefreshPidStatus: async () => calls.push('pid'),
        _dcRefreshMotorStatus: () => { throw Error('Initial navigation invoked a motor command'); }
      });
      await state._dcInitModbusDeviceStates(immediate);
      assert.deepEqual(calls, ['relay', 'pwm', 'pid']);
      assert.equal(state._dcModbusInitRunning, false);
      calls.length = 0;
      state._dcIsLifecycleActive = () => false;
      await state._dcInitModbusDeviceStates(immediate);
      assert.deepEqual(calls, [], 'Hidden/stale page must not continue probing');
      assert.equal(state._dcModbusInitRunning, false);
    }
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
"""
        result = subprocess.run(["node", "-e", script], cwd=ROOT, capture_output=True,
                                text=True, encoding="utf-8", timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stale_load_and_runtime_callbacks_are_generation_guarded(self):
        self.assertIn("var loadGeneration = ++this._dcLifecycleGeneration", CORE)
        self.assertIn("self._dcIsLifecycleActive(loadGeneration, content)", CORE)
        self.assertNotIn("self._tempControlsRes = controlsRes", CORE)
        self.assertNotIn("self._tempModbusRes = modbusRes", CORE)
        self.assertIn("var generation = this._dcLifecycleGeneration", MODBUS_OPS)
        self.assertGreaterEqual(
            MODBUS_CTRL.count("if (!self._dcIsLifecycleActive(generation)) return;"),
            20,
        )


if __name__ == "__main__":
    unittest.main()
