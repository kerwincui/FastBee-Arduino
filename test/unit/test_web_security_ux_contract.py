"""Contracts for Web DOM safety, page lifecycle, and lightweight publishing."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
DASHBOARD = (ROOT / "web-src/modules/runtime/dashboard.js").read_text(encoding="utf-8")
FULLSCREEN = (ROOT / "web-src/modules/runtime/dashboard-fullscreen.js").read_text(encoding="utf-8")
NETWORK = (ROOT / "web-src/modules/runtime/network.js").read_text(encoding="utf-8")
NOTIFICATION = (ROOT / "web-src/js/notification.js").read_text(encoding="utf-8")
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")
FEATURES = (ROOT / "web-src/modules/runtime/feature-config.js").read_text(encoding="utf-8")
USERS = (ROOT / "web-src/modules/admin/users.js").read_text(encoding="utf-8")
LOGS = (ROOT / "web-src/modules/admin/logs.js").read_text(encoding="utf-8")
PERIPHERALS = (ROOT / "web-src/modules/runtime/peripherals.js").read_text(encoding="utf-8")
PERIPHERAL_HTML = (ROOT / "web-src/pages/peripheral.html").read_text(encoding="utf-8")
BUILD_MODULES = (ROOT / "scripts/build-web-modules.js").read_text(encoding="utf-8")
BROWSER_SUITE = (
    ROOT / "test/browser/suites/27-web-security-lifecycle.spec.ts"
).read_text(encoding="utf-8")


class WebSecurityUxContractTests(unittest.TestCase):
    def test_dashboard_and_fullscreen_use_node_renderers_for_device_strings(self):
        self.assertIn("_renderDashboardConnectionStatus(", DASHBOARD)
        self.assertIn("_renderDashboardBadge(", DASHBOARD)
        self.assertIn("document.createTextNode(", DASHBOARD)
        self.assertNotIn("_setHtml(", DASHBOARD)
        self.assertIn("_renderConnectionStatus: function(", FULLSCREEN)
        self.assertIn("_renderBadge: function(", FULLSCREEN)
        self.assertIn("document.createTextNode(", FULLSCREEN)
        self.assertNotIn("_setHtml", FULLSCREEN)
        self.assertNotIn("_setHtml(id, html)", STATE)

    def test_notifications_have_no_html_mode_or_inner_html_sink(self):
        self.assertIn("body.textContent =", NOTIFICATION)
        self.assertNotIn("body.innerHTML", NOTIFICATION)
        self.assertNotIn("options.html", NOTIFICATION)
        self.assertNotIn("html: true", NETWORK)
        self.assertNotIn("<br>", NETWORK)
        self.assertIn("this._escapeHtml(detail)", NETWORK)
        self.assertIn("const encryptionEsc = this._escapeHtml(encryption)", NETWORK)

    def test_feature_developer_mode_renderer_does_not_replace_global_method(self):
        self.assertIn("applyDeveloperModeState(root)", STATE)
        self.assertIn("applyFeatureDeveloperModeState", STATE)
        self.assertIn("applyFeatureDeveloperModeState(scope)", FEATURES)
        self.assertNotIn("\n        applyDeveloperModeState(scope)", FEATURES)
        self.assertIn('data-feature-locked="true"', FEATURES)
        self.assertIn("disabled || featureLocked", FEATURES)

    def test_network_and_fullscreen_release_background_work(self):
        self.assertIn("_stopNetworkPageWork()", NETWORK)
        self.assertIn("AppState.onPageHide('network'", NETWORK)
        self.assertIn("_networkDeferredTimers", NETWORK)
        self.assertIn("_scheduleNetworkPageTask(", NETWORK)
        self.assertIn("document.addEventListener('visibilitychange'", FULLSCREEN)
        self.assertIn("window.addEventListener('pagehide'", FULLSCREEN)
        self.assertIn("destroy: function()", FULLSCREEN)
        self.assertIn("this._sseSource.close()", FULLSCREEN)

    def test_remounted_network_page_rebinds_secondary_controls_once(self):
        self.assertIn("pageRoot.dataset.eventsBound === 'true'", NETWORK)
        self.assertIn("this.setupStatusRefresh();", NETWORK)
        self.assertIn("this.setupBleProvision();", NETWORK)
        self.assertIn("this._filterNetworkTypesByCapabilities();", NETWORK)
        self.assertNotIn("const originalLoadConfig = this.loadNetworkConfig.bind(this)", NETWORK)

    def test_user_edit_and_log_loading_states_match_visible_workflow(self):
        self.assertIn("input.required = !isEditMode", USERS)
        self.assertIn("if (password) update.password = password", USERS)
        self.assertIn("modal.dataset.editEnabled", USERS)
        self.assertIn("return apiCall", USERS)
        self.assertEqual(1, USERS.count("确定要删除用户"))
        self.assertIn("return this.loadUsers({ noCache: true }).finally", USERS)
        self.assertIn("_logListLoadSeq", LOGS)
        self.assertIn("_logContentLoadSeq", LOGS)
        self.assertIn("this.currentPage !== 'logs'", LOGS)

    def test_peripheral_roots_are_balanced_and_duplicate_script_is_not_published(self):
        self.assertEqual(PERIPHERAL_HTML.count("<div"), PERIPHERAL_HTML.count("</div>"))
        self.assertIn("const STANDALONE_ROOT_JS = [];", BUILD_MODULES)
        self.assertIn("notification.js is already part of chunk-2-app.js", BUILD_MODULES)

    def test_runtime_managed_peripheral_types_stay_out_of_create_select(self):
        self.assertIn("item.configurable !== false && item.implemented !== false", PERIPHERALS)
        self.assertIn("item.configurable === false) return", PERIPHERALS)
        self.assertIn("dynamicGroup.remove()", PERIPHERALS)
        self.assertIn("typeSelect.value = selectedValue", PERIPHERALS)
        self.assertIn("option.hidden = !!item && !configurable", PERIPHERALS)

    def test_browser_regression_injects_api_markup_and_repeats_navigation(self):
        for test_id in ("WEB-SEC-001", "WEB-SEC-002", "WEB-SEC-003", "WEB-SEC-004"):
            self.assertIn(test_id, BROWSER_SUITE)
        self.assertIn("WEB-UX-001", BROWSER_SUITE)
        self.assertIn("WEB-UX-002", BROWSER_SUITE)
        self.assertIn("data-web-xss", BROWSER_SUITE)
        self.assertIn("for (let round = 0; round < 4; round++)", BROWSER_SUITE)
        self.assertIn("for (const width of [1280, 390])", BROWSER_SUITE)


if __name__ == "__main__":
    unittest.main()
