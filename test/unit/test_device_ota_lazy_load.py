import json
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MODULE = ROOT / "web-src" / "modules" / "runtime" / "device-config.js"


NODE_PROBE = r"""
const fs = require('fs');
const vm = require('vm');

const sourcePath = process.argv[1];
const exposeOtaTab = process.argv[2] !== 'without-ota';
const source = fs.readFileSync(sourcePath, 'utf8');
let otaClick = null;
let otaLoads = 0;
const otaTab = {
  _fastbeeOtaLazyBound: false,
  addEventListener: (name, callback) => {
    if (name === 'click') otaClick = callback;
  },
};
const AppState = {
  registerModule: function(_name, methods) { Object.assign(this, methods); },
};
const sandbox = {
  AppState,
  window: {},
  document: {
    getElementById: () => null,
    querySelector: selector => exposeOtaTab && selector.includes('data-tab="dev-ota"') ? otaTab : null,
  },
  console,
  setTimeout: () => 0,
  clearTimeout: () => {},
};
vm.runInNewContext(source, sandbox, { filename: sourcePath });
AppState._loadOtaFragment = () => { otaLoads++; };
AppState.setupDeviceConfigEvents();
const beforeClick = otaLoads;
if (otaClick) otaClick();
process.stdout.write(JSON.stringify({
  beforeClick,
  afterClick: otaLoads,
  listenerBound: typeof otaClick === 'function',
}));
"""


class DeviceOtaLazyLoadTests(unittest.TestCase):
    def test_ota_assets_load_only_after_ota_tab_click(self):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE, str(MODULE)],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        payload = json.loads(result.stdout)
        self.assertTrue(payload["listenerBound"])
        self.assertEqual(payload["beforeClick"], 0)
        self.assertEqual(payload["afterClick"], 1)

    def test_ota_disabled_build_does_not_bind_or_request_fragment(self):
        result = subprocess.run(
            ["node", "-e", NODE_PROBE, str(MODULE), "without-ota"],
            cwd=ROOT,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        payload = json.loads(result.stdout)
        self.assertFalse(payload["listenerBound"])
        self.assertEqual(payload["beforeClick"], 0)
        self.assertEqual(payload["afterClick"], 0)


if __name__ == "__main__":
    unittest.main()
