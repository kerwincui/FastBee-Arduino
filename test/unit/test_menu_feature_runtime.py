"""Execute menu status loading with partial/failing API responses and bounded requests."""

import json
from pathlib import Path
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
NODE_PROBE = r"""
const fs = require('fs');
const vm = require('vm');
const source = fs.readFileSync(process.argv[1], 'utf8');
const primary = { success: true, data: { features: {
  modbus: 'active', logViewer: 'active', userAdmin: 'active', screenPanel: 'inactive'
} } };
const panel = (compiled, userEnabled) => ({ success: true, data: { features: [
  { key: 'screenPanel', compiled, userEnabled, active: false }
] } });
async function probe(system, full, includePanel = true) {
  const calls = [];
  const menus = {};
  const getter = async path => {
    calls.push(path);
    if (calls.length > 2) throw new Error('Unexpected repeated feature status request');
    const response = path === '/api/system/status' ? system : full;
    if (response === 'reject') throw new Error('temporarily unavailable');
    return response;
  };
  const sandbox = {
    console, setTimeout, clearTimeout,
    apiGet: getter, apiGetSilentFresh: getter,
    document: {
      querySelector(selector) {
        const match = selector.match(/data-page="([^"]+)"/);
        if (!match) return null;
        return menus[match[1]] ||= { style: {}, setAttribute() {} };
      },
      getElementById() { return null; }
    }
  };
  sandbox.window = sandbox;
  vm.runInNewContext(source, sandbox);
  const state = sandbox.AppState;
  if (!includePanel) delete state._featureMenuMap.screenPanel;
  const pending = state.applyFeatureMenuVisibility();
  const deduplicated = pending === state.applyFeatureMenuVisibility();
  await pending;
  return {
    calls, deduplicated, ready: state._featureStatusReady,
    completed: state._featureStatusPromise === null,
    active: state._featureActive,
    visible: Object.fromEntries(Object.entries(menus).map(([key, value]) => [key, value.style.display !== 'none']))
  };
}
(async () => {
  const results = {
    fallback: await probe('reject', panel(true, true)),
    partial: await probe(primary, panel(true, true)),
    unavailable: await probe(primary, 'reject'),
    uncompiled: await probe(primary, panel(false, true)),
    disabled: await probe(primary, panel(true, false)),
    failed: await probe('reject', 'reject'),
    lightweight: await probe(primary, 'reject', false)
  };
  process.stdout.write(JSON.stringify(results));
})().catch(error => { console.error(error); process.exit(1); });
"""


class MenuFeatureRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE, str(ROOT / "web-src/js/state.js")],
            cwd=ROOT, check=True, capture_output=True, text=True, timeout=15,
        )
        cls.results = json.loads(result.stdout)

    def test_full_fallback_does_not_query_itself_again(self):
        result = self.results["fallback"]
        self.assertEqual(["/api/system/status", "/api/features/status"], result["calls"])
        self.assertTrue(result["active"]["screenPanel"])
        self.assertTrue(result["visible"]["screen-panel"])

    def test_panel_supplement_preserves_other_runtime_features(self):
        result = self.results["partial"]
        self.assertEqual(["/api/system/status", "/api/features/status"], result["calls"])
        for key in ("screenPanel", "modbus", "logViewer", "userAdmin"):
            self.assertTrue(result["active"][key], key)

    def test_failed_supplement_keeps_the_primary_state_without_retrying(self):
        result = self.results["unavailable"]
        self.assertEqual(2, len(result["calls"]))
        self.assertTrue(result["ready"])
        self.assertTrue(result["active"]["modbus"])
        self.assertFalse(result["active"]["screenPanel"])

    def test_panel_menu_requires_compiled_and_user_enabled(self):
        for name in ("uncompiled", "disabled"):
            with self.subTest(name=name):
                self.assertFalse(self.results[name]["active"]["screenPanel"])
                self.assertFalse(self.results[name]["visible"]["screen-panel"])

    def test_all_status_failures_keep_optional_menus_closed(self):
        result = self.results["failed"]
        self.assertFalse(result["ready"])
        self.assertIsNone(result["active"])
        self.assertFalse(result["visible"]["screen-panel"])
        self.assertTrue(result["visible"]["device"])

    def test_concurrent_loads_share_one_finite_operation(self):
        for name, result in self.results.items():
            with self.subTest(name=name):
                self.assertTrue(result["deduplicated"])
                self.assertTrue(result["completed"])
                self.assertLessEqual(len(result["calls"]), 2)

    def test_without_a_panel_menu_the_primary_status_remains_sufficient(self):
        self.assertEqual(["/api/system/status"], self.results["lightweight"]["calls"])


if __name__ == "__main__":
    unittest.main()
