"""Source-level regression checks for the peripheral Web form contract."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODALS = (ROOT / "web-src/pages/modals.html").read_text(encoding="utf-8")
PERIPHERALS = (ROOT / "web-src/modules/runtime/peripherals.js").read_text(
    encoding="utf-8"
)
PERIPHERAL_SAVE = (ROOT / "web-src/modules/runtime/peripheral-save.js").read_text(
    encoding="utf-8"
)
PERIPHERAL_IO = (ROOT / "web-src/modules/runtime/peripheral-io.js").read_text(
    encoding="utf-8"
)


def element_tag(element_id: str) -> str:
    match = re.search(rf"<[^>]+\bid=\"{re.escape(element_id)}\"[^>]*>", MODALS)
    if match is None:
        raise AssertionError(f"missing element #{element_id}")
    return match.group(0)


class PeripheralWebContractTests(unittest.TestCase):
    def test_optional_save_and_diagnostics_workflows_are_lazy_loaded(self):
        self.assertNotIn("/api/boards/profiles", PERIPHERALS)
        self.assertNotIn("/api/peripherals/resources", PERIPHERALS)
        self.assertIn("ModuleLoader.loadModule('peripheral-save', run)", PERIPHERALS)
        self.assertIn("ModuleLoader.loadModule('peripheral-io', run)", PERIPHERALS)
        self.assertIn("registerModule('peripheral-save'", PERIPHERAL_SAVE)
        self.assertIn("registerModule('peripheral-io'", PERIPHERAL_IO)

    def test_initial_markup_is_fail_closed_and_new_devices_are_disabled(self):
        self.assertRegex(element_tag("peripheral-type-input"), r"\bdisabled\b")
        self.assertRegex(element_tag("save-peripheral-btn"), r"\bdisabled\b")
        self.assertNotRegex(element_tag("peripheral-enabled-input"), r"\bchecked\b")
        self.assertIn("if (enabledCb) enabledCb.checked = false", PERIPHERALS)

        for value in ("50", "52"):
            match = re.search(rf'<option\s+value="{value}"[^>]*>', MODALS)
            self.assertIsNotNone(match)
            self.assertRegex(match.group(0), r"\bdisabled\b")
            self.assertIn('data-implemented="false"', match.group(0))

    def test_type_catalog_drives_options_and_save_gate(self):
        self.assertIn("apiGet('/api/peripherals/types')", PERIPHERALS)
        self.assertIn("this._periphTypeCatalog = catalog", PERIPHERALS)
        self.assertIn("_syncPeripheralTypeSelect(catalog)", PERIPHERALS)
        self.assertIn("item.implemented !== false", PERIPHERALS)
        self.assertIn("option.disabled = !implemented", PERIPHERALS)
        self.assertIn("typeSelect.disabled = !ready", PERIPHERALS)

        gate = PERIPHERAL_SAVE.index("if (!this._isPeripheralTypeAvailable(typeNum))")
        post = PERIPHERAL_SAVE.index("apiPost(url, data)")
        self.assertLess(gate, post)

    def test_capability_failure_disables_every_type_and_prompts_retry(self):
        self.assertGreaterEqual(PERIPHERALS.count("this._syncPeripheralTypeSelect(null)"), 2)
        sync_match = re.search(
            r"_syncPeripheralTypeSelect\(catalog\) \{([\s\S]*?)\n\s*\},\n\n\s*_getPinRecommendation",
            PERIPHERALS,
        )
        self.assertIsNotNone(sync_match)
        sync_body = sync_match.group(1)
        self.assertIn("const configurable = !!item", sync_body)
        self.assertIn("const implemented = configurable", sync_body)
        self.assertIn("option.disabled = !implemented", sync_body)
        self.assertIn("option.hidden = !!item && !configurable", sync_body)
        self.assertIn("item ? 'false' : 'unknown'", sync_body)
        self.assertIn("设备外设能力尚未加载，请稍后重试", PERIPHERALS)
        self.assertIn("if (!Array.isArray(this._periphTypeCatalog) && !this._periphTypeCatalogLoading)", PERIPHERALS)

    def test_servo_form_has_safe_defaults(self):
        defaults = {
            "servo-frequency": "50",
            "servo-min-pulse": "500",
            "servo-max-pulse": "2500",
            "servo-min-angle": "0",
            "servo-max-angle": "180",
            "servo-initial-angle": "90",
        }
        for element_id, value in defaults.items():
            self.assertIn(f'value="{value}"', element_tag(element_id))
        self.assertNotRegex(element_tag("servo-inverted"), r"\bchecked\b")
        self.assertNotRegex(element_tag("servo-release-after-move"), r"\bchecked\b")

    def test_servo_type_switch_edit_and_save_are_complete(self):
        all_web = PERIPHERALS + PERIPHERAL_SAVE
        self.assertRegex(
            PERIPHERALS,
            r"type === 41[\s\S]*?getElementById\('servo-params'\)",
        )
        for field in (
            "frequency",
            "minPulse",
            "maxPulse",
            "minAngle",
            "maxAngle",
            "initialAngle",
            "inverted",
            "releaseAfterMove",
        ):
            self.assertIn(field, all_web)

        save_match = re.search(
            r"else if \(typeNum === 41\) \{([\s\S]*?)\n\s*\} else if \(typeNum === 47\)",
            PERIPHERAL_SAVE,
        )
        self.assertIsNotNone(save_match)
        save_body = save_match.group(1)
        for field in (
            "frequency",
            "minPulse",
            "maxPulse",
            "minAngle",
            "maxAngle",
            "initialAngle",
            "inverted",
            "releaseAfterMove",
        ):
            self.assertIn(f"data.{field}", save_body)
        self.assertIn("servoMaxPulse >= (1000000 / servoFrequency)", save_body)
        self.assertIn("servoInitialAngle < servoMinAngle", save_body)

    def test_enable_toggle_is_preflighted_without_nonexistent_toggle_route(self):
        self.assertNotIn("/api/peripherals/toggle", PERIPHERALS)
        status = PERIPHERALS.index("'/api/peripherals/status'")
        preflight = PERIPHERALS.index("'/api/peripherals/preflight'", status)
        enable = PERIPHERALS.index("'/api/peripherals/enable'", preflight)
        self.assertLess(status, preflight)
        self.assertLess(preflight, enable)
        self.assertIn("operation: 'enable'", PERIPHERALS)
        self.assertIn("payload.acknowledgements = acknowledgements", PERIPHERALS)
        self.assertIn("'/api/peripherals/disable'", PERIPHERALS)
        self.assertIn("if (!confirm('启用前预检提示", PERIPHERALS)


if __name__ == "__main__":
    unittest.main()
