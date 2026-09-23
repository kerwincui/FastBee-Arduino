"""Contracts for shared responsive tables and neutral async status states."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
CSS = (ROOT / "web-src/css/main.css").read_text(encoding="utf-8")
UI = (ROOT / "web-src/js/ui-components.js").read_text(encoding="utf-8")
FEATURE_HTML = (ROOT / "web-src/pages/features.html").read_text(encoding="utf-8")
FEATURE_JS = (ROOT / "web-src/modules/runtime/feature-config.js").read_text(encoding="utf-8")
NETWORK_HTML = (ROOT / "web-src/pages/network.html").read_text(encoding="utf-8")
NETWORK_JS = (ROOT / "web-src/modules/runtime/network.js").read_text(encoding="utf-8")
MQTT_HTML = (ROOT / "web-src/pages/fragments/protocol-mqtt.html").read_text(encoding="utf-8")
MQTT_JS = (ROOT / "web-src/modules/runtime/protocol/mqtt-config.js").read_text(encoding="utf-8")
UI_SUITE = (ROOT / "test/browser/suites/15-ui-regression.spec.ts").read_text(encoding="utf-8")
DEVICE_HTML = (ROOT / "web-src/pages/device.html").read_text(encoding="utf-8")
OTA_HTML = (ROOT / "web-src/pages/fragments/device-ota.html").read_text(encoding="utf-8")
PAGE_HTML = {
    name: (ROOT / "web-src/pages" / name).read_text(encoding="utf-8")
    for name in (
        "admin.html",
        "device.html",
        "logs.html",
        "peripheral.html",
        "protocol.html",
        "rule-script.html",
    )
}


class UiResponsiveStatusContractTests(unittest.TestCase):
    def test_shared_table_enhancer_is_scoped_to_management_tables(self):
        self.assertIn("enhanceResponsiveTables", UI)
        self.assertIn("!hasActions && !table.classList.contains('fb-table-mobile')", UI)
        self.assertIn("fb-table-responsive", CSS)
        self.assertIn("content: attr(data-label)", CSS)
        self.assertNotIn(":has(.fb-table-responsive)", CSS)

    def test_mobile_matrix_covers_target_widths(self):
        self.assertIn("[360, 390, 430]", UI_SUITE)
        self.assertIn("pageOverflow", UI_SUITE)
        self.assertIn("tableOverflow", UI_SUITE)

    def test_mobile_crud_actions_use_one_accessible_menu(self):
        self.assertIn("enhanceMobileActionMenus", UI)
        self.assertIn("fb-table-action-menu-toggle", UI)
        self.assertIn("aria-haspopup', 'menu'", UI)
        self.assertIn("event.key === 'Escape'", UI)
        self.assertIn("event.key === 'ArrowDown'", UI)
        self.assertIn("fb-table-responsive-container", CSS)
        self.assertIn(".u-overflow-x, .u-overflow-x-auto", UI)
        self.assertIn("fb-table-action-cell--compact", UI)
        self.assertIn("td.fb-table-action-cell--compact::before { content: none; }", CSS)
        self.assertIn("fb-table-action-menu-panel button", CSS)
        self.assertIn("max-width: min(240px, calc(100vw - 32px))", CSS)
        self.assertIn("width: 40px", CSS)
        self.assertIn("min-height: 40px", CSS)

    def test_feature_resources_are_visible_in_the_primary_row(self):
        self.assertIn("<th>功能</th><th>状态</th><th>Flash</th><th>RAM</th><th>依赖</th><th>启用</th>", FEATURE_JS)
        self.assertIn("fb-table-mobile", FEATURE_JS)
        self.assertNotIn('<details class="feature-details">', FEATURE_JS)
        self.assertIn('data-feature-resource="flash"', FEATURE_JS)
        self.assertIn('data-feature-resource="ram"', FEATURE_JS)
        self.assertIn('data-feature-resource="dependency"', FEATURE_JS)
        self.assertNotIn('id="network-title"', NETWORK_HTML)
        self.assertNotIn('<h2 class="fb-card-title">功能配置</h2>', FEATURE_HTML)

    def test_network_initial_state_is_neutral_until_real_result(self):
        self.assertIn('status-badge status-detecting">检测中', NETWORK_HTML)
        self.assertIn("_setNetworkStatusLoading()", NETWORK_JS)
        self.assertIn("_showNetworkStatusFailure(reason)", NETWORK_JS)

    def test_mqtt_initial_state_is_neutral_until_real_result(self):
        self.assertIn("mqtt-status-detecting\">检测中", MQTT_HTML)
        self.assertIn("_setMqttStatusLoading()", MQTT_JS)
        self.assertIn("if (this._mqttApiFailCount < 2) return", MQTT_JS)

    def test_file_choosers_reuse_the_shared_button_component(self):
        self.assertIn(".file-input-wrapper", CSS)
        self.assertIn(".file-input-wrapper > label.fb-btn", CSS)
        self.assertIn(".file-name-display", CSS)
        for source, input_id in ((DEVICE_HTML, "dev-ota-file"),
                                 (DEVICE_HTML, "dev-config-import-file"),
                                 (OTA_HTML, "ota-file")):
            marker = 'id="' + input_id + '"'
            position = source.index(marker)
            container_start = source.rfind('class="file-input-wrapper', 0, position)
            container_end = source.find("</div>", position)
            self.assertGreater(container_start, 0, input_id + " must use file-input-wrapper")
            self.assertGreater(container_end, position)
            container = source[container_start:container_end]
            self.assertIn("fb-btn", container, input_id + " must have a shared button trigger")
            self.assertIn("file-name-display", container, input_id + " must expose the selected file name")

    def test_page_header_is_the_single_source_for_page_titles(self):
        for name, source in PAGE_HTML.items():
            self.assertNotIn(
                'class="fb-card-title"',
                source,
                name + " must not repeat the global page title inside the content card",
            )
        for name, summary_id in (
            ("admin.html", "users-list-summary"),
            ("logs.html", "log-meta"),
            ("rule-script.html", "rule-script-list-summary"),
        ):
            self.assertIn("fb-data-toolbar-leading", PAGE_HTML[name])
            self.assertIn('id="' + summary_id + '"', PAGE_HTML[name])
            self.assertIn("fb-card-header-actions", PAGE_HTML[name])
        self.assertIn("fb-data-toolbar-leading", PAGE_HTML["peripheral.html"])
        self.assertIn('id="peripheral-list-summary"', PAGE_HTML["peripheral.html"])
        self.assertIn("fb-card-header-actions", PAGE_HTML["peripheral.html"])

    def test_shared_visual_tokens_keep_controls_quiet_and_consistent(self):
        self.assertIn(".fb-btn {", CSS)
        self.assertIn("min-height: 36px", CSS)
        self.assertIn(".fb-card { border: 1px solid var(--border-secondary) !important; box-shadow: none !important; }", CSS)
        self.assertIn("body { background: var(--bg-body); }", CSS)
        self.assertNotIn("body { background-image: radial-gradient", CSS)

    def test_modal_body_contains_long_dynamic_content(self):
        modal_body_rule = CSS[CSS.index(".modal-body {"):CSS.index("}", CSS.index(".modal-body {"))]
        self.assertIn("min-width: 0", modal_body_rule)
        self.assertIn("overflow-x: hidden", modal_body_rule)
        self.assertIn("overflow-wrap: anywhere", modal_body_rule)
        self.assertIn("repeat(2, minmax(0, 1fr))", CSS)
        self.assertIn(".config-form-column, .modal-body > .fb-form { min-width: 0; max-width: 100%; }", CSS)
        self.assertIn(".modal-body .form-hint", CSS)
        self.assertIn("word-break: break-word", CSS)
        self.assertIn("grid-template-columns: minmax(0, 1fr) !important", CSS)
        self.assertIn("UI-007D: 360px 长动态内容模态框无横向溢出 @quick", UI_SUITE)


if __name__ == "__main__":
    unittest.main()
