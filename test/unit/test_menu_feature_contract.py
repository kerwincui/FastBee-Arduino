"""Regression checks for runtime feature-driven sidebar visibility."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")
STATE_SESSION = (ROOT / "web-src/js/state-session.js").read_text(encoding="utf-8")
BASE_FIXTURE = (ROOT / "test/browser/fixtures/base.fixture.ts").read_text(encoding="utf-8")
PLAYWRIGHT_CONFIG = (ROOT / "test/browser/playwright.config.ts").read_text(encoding="utf-8")
FEATURE_SUITE = (ROOT / "test/browser/suites/22-feature-config.spec.ts").read_text(encoding="utf-8")
FEATURE_CONFIG = (ROOT / "web-src/modules/runtime/feature-config.js").read_text(encoding="utf-8")
RUNTIME_FEATURES = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")
SYSTEM_ROUTES = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(encoding="utf-8")
LOG_ROUTE = (ROOT / "src/network/handlers/LogRouteHandler.cpp").read_text(encoding="utf-8")
RULE_ROUTE = (ROOT / "src/network/handlers/RuleScriptRouteHandler.cpp").read_text(encoding="utf-8")
USER_ROUTE = (ROOT / "src/network/handlers/UserRouteHandler.cpp").read_text(encoding="utf-8")
PERIPH_EXECUTOR = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")
FLEET_RUNNER = (ROOT / "scripts/run-device-fleet-tests.ps1").read_text(encoding="utf-8")
MAIN_CSS = (ROOT / "web-src/css/main.css").read_text(encoding="utf-8")
WEB_SMOKE = (ROOT / "scripts/web-smoke-test.js").read_text(encoding="utf-8")
FACTORY_FEATURES = (ROOT / "test/fixtures/factory-config/features.json").read_text(encoding="utf-8")


class MenuFeatureContractTests(unittest.TestCase):
    def test_only_optional_pages_are_feature_gated(self):
        match = re.search(r"_featureMenuMap\s*:\s*\{(.*?)\n\s*\},", STATE, re.S)
        self.assertIsNotNone(match)
        mapping = match.group(1)
        for key in ("ruleScript", "deviceControl", "logViewer", "userAdmin"):
            self.assertIn(key, mapping)
        self.assertNotIn("fileManager", mapping)
        for page in ("dashboard", "device", "network", "peripheral",
                     "periph-exec", "protocol", "features"):
            self.assertNotIn("'" + page + "'", mapping)

    def test_optional_menus_start_hidden_and_device_menu_is_preserved(self):
        self.assertIn("const menuPages = featureKeys.map(function(key) { return map[key]; });", STATE)
        self.assertIn("const pageToFeature = {};", STATE)
        self.assertIn("applyMenuVisibility(pending);", STATE)
        self.assertNotIn("applyMenuState(pending);", STATE)
        self.assertIn("setMenuVisible('device', true);", STATE)
        self.assertIn("item.style.display = visible ? '' : 'none';", STATE)

    def test_optional_menus_are_hidden_in_static_markup(self):
        index_html = (ROOT / "web-src/index.html").read_text(encoding="utf-8")
        for page in ("device-control", "rule-script", "logs", "users"):
            marker = f'data-page="{page}"'
            self.assertIn(marker, index_html)
            item = index_html[index_html.index(marker) - 120:index_html.index(marker) + 80]
            self.assertIn('style="display:none"', item)
            self.assertIn('aria-hidden="true"', item)

    def test_removed_file_manager_page_has_no_web_route_or_test_project(self):
        self.assertFalse((ROOT / "web-src/modules/admin/files.js").exists())
        self.assertNotIn("'data-page': 'admin'", STATE)
        self.assertNotIn("data: 'files'", STATE)
        self.assertNotIn("page-title-data", STATE)
        self.assertNotIn("12-files", PLAYWRIGHT_CONFIG)
        self.assertNotIn(".file-tree-", MAIN_CSS)
        self.assertNotIn("fileManager", BASE_FIXTURE)
        self.assertNotIn("fileManager", FACTORY_FEATURES)
        self.assertIn("'js/modules/files.js'", WEB_SMOKE)
        self.assertIn("'js/modules/files.js.gz'", WEB_SMOKE)
        self.assertIn("checkRetiredAssetsAreAbsent();", WEB_SMOKE)
        self.assertIn("retired asset must not be published", WEB_SMOKE)

    def test_authenticated_status_requires_compiled_and_active(self):
        self.assertIn("f.compiled !== false && f.active === true", STATE)
        self.assertIn("if (f && f.key)", STATE)

    def test_log_and_script_features_are_bundled_for_users(self):
        self.assertRegex(
            RUNTIME_FEATURES,
            r'registerFeature\(\{"commandScript",\s*nullptr,[\s\S]{0,180}?"ruleScript"',
        )
        self.assertRegex(
            RUNTIME_FEATURES,
            r'registerFeature\(\{"fileLogging",\s*nullptr,[\s\S]{0,180}?"logViewer"',
        )
        self.assertIn('registerFeature({"logViewer", "设备日志"', RUNTIME_FEATURES)
        self.assertIn("getAttachedFeatures(key)", FEATURE_CONFIG)
        self.assertIn("child.userEnabled = enabled;", FEATURE_CONFIG)

    def test_combined_feature_switches_gate_backend_behavior(self):
        self.assertGreaterEqual(LOG_ROUTE.count("requireDeviceLogs(ctx, request)"), 4)
        self.assertGreaterEqual(RULE_ROUTE.count("requireRuleEngine(ctx, request)"), 6)
        self.assertGreaterEqual(USER_ROUTE.count("requireUserAdministration(ctx, request)"), 7)
        self.assertIn('shouldEnable("userAdmin")', USER_ROUTE)
        self.assertIn('shouldEnable("commandScript")', PERIPH_EXECUTOR)

    def test_system_status_is_lightweight_primary(self):
        self.assertIn("const request = featureGetter('/api/system/status')", STATE)
        system_pos = STATE.index("const request = featureGetter('/api/system/status')")
        full_pos = STATE.index("return featureGetter('/api/features/status')", system_pos)
        self.assertLess(system_pos, full_pos)
        self.assertIn("states[key] === 'active'", STATE)
        self.assertIn("Object.keys(states).forEach(function(key)", STATE)

    def test_user_disabled_features_are_not_reported_as_no_memory(self):
        start = SYSTEM_ROUTES.index("功能运行时状态")
        end = SYSTEM_ROUTES.index("if (ctx->networkManager)", start)
        block = SYSTEM_ROUTES[start:end]
        user_enabled = block.index("rfm.isUserEnabled(meta->key)")
        runtime_disabled = block.index("rfm.isRuntimeDisabled(meta->key)")
        budget_denied = block.index("budgetReg.isDenied(meta->key)")
        self.assertLess(user_enabled, runtime_disabled)
        self.assertLess(user_enabled, budget_denied)
        self.assertIn("(!meta->compiled || !userEnabled) ? \"disabled\"", block)

    def test_dashboard_module_waits_for_menu_status(self):
        boot_start = STATE_SESSION.index("AppState._bootDashboardAfterAuth = async function()")
        boot_end = STATE_SESSION.index("AppState.handleLogin = function()", boot_start)
        boot = STATE_SESSION[boot_start:boot_end]
        status_pos = boot.index("await this.applyFeatureMenuVisibility()")
        dashboard_pos = boot.index("this._loadModule('dashboard'")
        self.assertLess(status_pos, dashboard_pos)

    def test_protocol_subtab_fails_closed_until_modbus_is_active(self):
        self.assertIn("_featureStatusReady: false", STATE)
        self.assertIn("const ready = this._featureStatusReady === true", STATE)
        self.assertIn("active.modbus === true", STATE)
        self.assertIn("document.getElementById('modbus-rtu')", STATE)
        self.assertIn("tab.style.display = enabled ? '' : 'none';", STATE)

    def test_protocol_page_refreshes_feature_status_after_startup_failure(self):
        loader_start = STATE.index("protocol: () => {")
        loader_end = STATE.index("logs: () =>", loader_start)
        loader = STATE[loader_start:loader_end]
        self.assertIn("const featureStatus = this.applyFeatureMenuVisibility()", loader)
        self.assertIn("return featureStatus", loader)
        self.assertIn("不要把待定值写入功能状态缓存", STATE)

    def test_status_failures_do_not_restore_all_menus(self):
        failure_block = re.search(
            r"首次请求失败时保持 fail-closed.*?applyMenuVisibility\(pending\);",
            STATE,
            re.S,
        )
        self.assertIsNotNone(failure_block)

    def test_fleet_quick_includes_live_menu_visibility_regression(self):
        for project in ("smoke", "features"):
            block = re.search(
                rf"name:\s*'{project}'.*?testMatch:\s*/([^/]+)/",
                PLAYWRIGHT_CONFIG,
                re.S,
            )
            self.assertIsNotNone(block)
            self.assertIn("22-feature-config", block.group(1))
        self.assertIn("FCFG-012: 可选菜单可见性与 active 状态一致 @quick", FEATURE_SUITE)
        self.assertIn("feature?.compiled !== false && feature?.active === true", FEATURE_SUITE)
        self.assertIn("Object.values(OPTIONAL_FEATURE_MENUS)", FEATURE_SUITE)
        self.assertIn("testInfo.project.name === 'smoke'", FEATURE_SUITE)
        self.assertRegex(FLEET_RUNNER, r'-Project\s+smoke\s+-Grep\s+"@quick"')

    def test_browser_navigation_rejects_hidden_preloaded_pages(self):
        self.assertIn("style.display === 'none'", BASE_FIXTURE)
        self.assertIn("style.visibility === 'hidden'", BASE_FIXTURE)
        self.assertIn("el.getBoundingClientRect().width === 0", BASE_FIXTURE)
        self.assertIn("#features-page', { state: 'visible'", FEATURE_SUITE)
        self.assertIn("#features-refresh-btn', { state: 'visible'", FEATURE_SUITE)

    def test_all_features_are_rendered_without_a_zero_ram_section(self):
        self.assertIn("this.renderGroupTables(this._featureState)", FEATURE_CONFIG)
        self.assertNotIn("data-zero-ram-toggle", FEATURE_CONFIG)
        self.assertNotIn("features-zero-ram-body", FEATURE_CONFIG)
        self.assertNotIn("features-unified-hint", FEATURE_CONFIG)
        self.assertIn("status.features.filter(f => !!f.name)", FEATURE_SUITE)


if __name__ == "__main__":
    unittest.main()
