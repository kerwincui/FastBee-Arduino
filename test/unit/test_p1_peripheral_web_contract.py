"""P1 source-level contracts for the peripheral Web workspace."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGE = (ROOT / "web-src/pages/peripheral.html").read_text(encoding="utf-8")
MODALS = (ROOT / "web-src/pages/modals.html").read_text(encoding="utf-8")
WEB = (ROOT / "web-src/modules/runtime/peripherals.js").read_text(encoding="utf-8")
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")
SAVE = (ROOT / "web-src/modules/runtime/peripheral-save.js").read_text(encoding="utf-8")
IO = (ROOT / "web-src/modules/runtime/peripheral-io.js").read_text(encoding="utf-8")
IO_MODAL = (ROOT / "web-src/pages/fragments/peripheral-io.html").read_text(
    encoding="utf-8"
)
ALL_WEB = WEB + SAVE + IO


def method_body(source: str, name: str, next_name: str) -> str:
    pattern = rf"\n\s*{re.escape(name)}\([^)]*\) \{{([\s\S]*?)\n\s*{re.escape(next_name)}\("
    match = re.search(pattern, source)
    if match is None:
        raise AssertionError(f"missing method boundary {name} -> {next_name}")
    return match.group(1)


class P1PeripheralWebContractTests(unittest.TestCase):
    def test_workspace_only_exposes_peripheral_configuration(self):
        self.assertIn('id="peripheral-page"', PAGE)
        self.assertIn('id="peripheral-table-body"', PAGE)
        self.assertIn('id="periph-exec-page"', PAGE)
        self.assertNotIn('class="config-tabs"', PAGE)
        for removed_surface in (
            "peripheral-board-resources",
            "peripheral-automation",
            "peripheral-experiments",
        ):
            self.assertNotIn(removed_surface, PAGE)
        self.assertNotIn("peripheral-board", WEB)
        self.assertNotIn("peripheral-experiments", STATE)
        self.assertFalse((ROOT / "web-src/modules/runtime/peripheral-board.js").exists())
        self.assertFalse((ROOT / "web-src/modules/runtime/peripheral-experiments.js").exists())

    def test_descriptor_drives_dynamic_types_and_role_order(self):
        self.assertIn("document.createElement('option')", WEB)
        self.assertIn("item.implemented !== false", WEB)
        self.assertIn("option.disabled = !implemented", WEB)
        self.assertIn("descriptor.pinRoles || descriptor.roles", WEB)
        for roles in (
            "1: ['RX', 'TX']",
            "37: document.getElementById('sdcard-interface')",
            "43: ['A', 'B']",
            "50: ['CE', 'IO', 'SCLK']",
            "52: ['SDA', 'SCL']",
            "54: ['SCK', 'MOSI', 'MISO', 'SS', 'RST']",
        ):
            self.assertIn(roles, WEB)

        rfid_option = re.search(r'<option value="54"[^>]*>', MODALS)
        self.assertIsNotNone(rfid_option)
        self.assertIn("disabled", rfid_option.group(0))
        self.assertIn('data-implemented="false"', rfid_option.group(0))

    def test_generic_i2c_driver_fields_round_trip(self):
        for element_id in (
            "generic-sensor-driver",
            "generic-sensor-bus-id",
            "generic-sensor-address",
            "generic-sensor-channel",
        ):
            self.assertIn(f'id="{element_id}"', MODALS)
        for field in (
            "data.driverName",
            "data.busPeripheralId",
            "data.i2cAddress",
            "data.params.driverName",
            "data.params.busPeripheralId",
            "data.params.i2cAddress",
        ):
            self.assertIn(field, ALL_WEB)
        self.assertIn("descriptor.implemented !== true", WEB)
        self.assertIn("const selectedChannel", SAVE)
        self.assertIn("Number(savedParams.mode || 0) + 1", WEB)

    def test_preflight_precedes_crud_and_only_known_absence_downgrades(self):
        save = SAVE.index("async savePeripheralConfig()")
        preflight = SAVE.index("await this._runPeripheralPreflight(data)", save)
        post = SAVE.index("apiPost(url, data)", save)
        self.assertLess(preflight, post)
        self.assertIn("'/api/peripherals/preflight'", SAVE)
        unsupported = method_body(
            SAVE, "_isUnsupportedPeripheralPreflight", "_normalizePeripheralPreflight"
        )
        for status in (404, 405, 501):
            self.assertIn(f"status === {status}", unsupported)
        self.assertNotIn("status === 400", unsupported)
        self.assertIn("if (!result.allowed)", SAVE)
        self.assertIn("result.requiresConfirmation", SAVE)
        self.assertIn("data.confirmWarnings = 1", SAVE)
        self.assertIn("data.acknowledgements = this._periphPreflightAcknowledgements", SAVE)
        self.assertIn("this._periphPreflightFingerprint === fingerprint", SAVE)
        self.assertIn("t('peri-confirm-save', '确认并保存')", SAVE)
        self.assertIn("请再次点击“确认并保存”", SAVE)
        self.assertNotIn("peripheral-preflight-confirm", MODALS + WEB + SAVE)
        self.assertIn("data.valid !== false", SAVE)
        self.assertIn("data.errors", SAVE)
        self.assertIn("data.warnings", SAVE)
        self.assertIn("level === 'warning'", SAVE)
        self.assertIn("ModuleLoader.loadModule('peripheral-save', run)", WEB)

    def test_uart_polling_is_bounded_and_cleaned_up(self):
        self.assertIn("setInterval(() =>", IO)
        self.assertIn("clearInterval(this._periphIoPollTimer)", IO)
        self.assertIn(
            "typeof this._stopPeripheralIoPolling === 'function'",
            WEB,
        )
        close = method_body(IO, "closePeripheralIoModal", "_renderPeripheralIoReading")
        self.assertIn("this._stopPeripheralIoPolling()", close)
        self.assertIn("combined.length > 8192", IO)
        self.assertIn("clearPeripheralUartOutput()", IO)
        self.assertIn("PageLoader.loadFragment('', 'peripheral-io')", IO)
        self.assertNotIn('id="peripheral-io-modal"', MODALS)
        self.assertIn('id="peripheral-io-modal"', IO_MODAL)

    def test_sd_and_rfid_actions_match_safe_backend_contract(self):
        for action in ("mount", "unmount", "write-file", "delete-file"):
            self.assertIn(f"'{action}'", IO)
        self.assertIn("confirm: normalized === 'delete-file' ? 'DELETE' : ''", IO)
        self.assertIn("action: 'scan'", IO)
        self.assertIn("action === 'read-block' || action === 'write-block'", IO)
        self.assertIn("confirm: action === 'write-block' ? 'WRITE_BLOCK' : ''", IO)
        self.assertIn("if (keyInput) keyInput.value = ''", IO)
        self.assertNotRegex(IO, r"apiGet(?:Fresh)?\([^\n]+key\s*:")

    def test_i2c_ds1302_and_lcd1602_panels_are_complete_and_gated(self):
        for panel in (
            "peripheral-i2c-sensor-panel",
            "peripheral-ds1302-panel",
            "peripheral-lcd1602-panel",
        ):
            self.assertIn(f'id="{panel}"', IO_MODAL)
        self.assertIn("descriptor.implemented === true", WEB)
        for action in (
            "'set-datetime'",
            "'set-running'",
            "action: 'print'",
            "payload.action = 'backlight'",
            "payload.enabled",
        ):
            self.assertIn(action, IO)
        self.assertIn("LCD1602 文本仅支持可打印 ASCII 字符", IO)

    def test_p3_capabilities_have_no_execution_entry(self):
        self.assertNotIn("/api/ble", ALL_WEB)
        self.assertNotIn("/api/peripherals/tcp", ALL_WEB)
        self.assertNotIn("/api/peripherals/udp", ALL_WEB)
        self.assertNotIn("oled-draw", ALL_WEB.lower())


if __name__ == "__main__":
    unittest.main()
