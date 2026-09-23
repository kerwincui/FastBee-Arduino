import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")
PAGE_LOADER = (ROOT / "web-src/js/page-loader.js").read_text(encoding="utf-8")
CORE = (ROOT / "web-src/modules/runtime/device-control/core.js").read_text(encoding="utf-8")
MODBUS_OPS = (ROOT / "web-src/modules/runtime/device-control/modbus-ops.js").read_text(encoding="utf-8")
MODBUS_CTRL = (ROOT / "web-src/modules/runtime/device-control/modbus-ctrl.js").read_text(encoding="utf-8")


class DeviceControlLifecycleContractTests(unittest.TestCase):
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
        init_start = CORE.index("_dcInitModbusDeviceStates: async function()")
        init_end = CORE.index("_dcStopAllAutoRefresh: function()", init_start)
        init_body = CORE[init_start:init_end]
        self.assertNotIn("_dcRefreshMotorStatus", init_body)
        self.assertIn("explicit refresh button", init_body)
        self.assertIn("_dcRefreshMotorStatus: function(devIdx)", MODBUS_CTRL)

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
