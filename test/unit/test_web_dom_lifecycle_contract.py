"""Contracts for bounded page DOM and repeatable lifecycle registration."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGE_LOADER = (ROOT / "web-src/js/page-loader.js").read_text(encoding="utf-8")
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")
STATE_SESSION = (ROOT / "web-src/js/state-session.js").read_text(encoding="utf-8")
UI_SUITE = (ROOT / "test/browser/suites/15-ui-regression.spec.ts").read_text(encoding="utf-8")
MODBUS_SUITE = (ROOT / "test/browser/suites/08-modbus.spec.ts").read_text(encoding="utf-8")
BASE_FIXTURE = (ROOT / "test/browser/fixtures/base.fixture.ts").read_text(encoding="utf-8")
PERIPH_EXEC = (ROOT / "web-src/modules/runtime/periph-exec.js").read_text(encoding="utf-8")
PERIPH_EXEC_FORM = (
    ROOT / "web-src/modules/runtime/periph-exec-form.js"
).read_text(encoding="utf-8")
PERIPH_EXEC_EDITOR_PANELS = (
    ROOT / "web-src/modules/runtime/periph-exec-templates.js"
).read_text(encoding="utf-8")
PERIPH_EXEC_MODBUS = (
    ROOT / "web-src/modules/runtime/periph-exec-modbus.js"
).read_text(encoding="utf-8")
PERIPH_EXEC_SENSOR = (
    ROOT / "web-src/modules/runtime/periph-exec-sensor.js"
).read_text(encoding="utf-8")
PERIPH_EXEC_EFFECTS = (
    ROOT / "web-src/modules/runtime/periph-exec-effects.js"
).read_text(encoding="utf-8")
MODULE_LOADER = (ROOT / "web-src/js/module-loader.js").read_text(encoding="utf-8")
DEVICE_CONTROL = (
    ROOT / "web-src/modules/runtime/device-control/core.js"
).read_text(encoding="utf-8")
DEVICE_CONTROL_LAYOUT = (
    ROOT / "web-src/modules/runtime/device-control/layout.js"
).read_text(encoding="utf-8")


def _call_arguments(source, marker):
    calls = []
    cursor = 0
    while True:
        call_start = source.find(marker, cursor)
        if call_start < 0:
            return calls
        index = call_start + len(marker)
        part_start = index
        parts = []
        paren_depth = 1
        brace_depth = 0
        bracket_depth = 0
        quote = None
        escaped = False
        line_comment = False
        block_comment = False
        while index < len(source):
            char = source[index]
            following = source[index + 1] if index + 1 < len(source) else ""
            if line_comment:
                if char == "\n":
                    line_comment = False
                index += 1
                continue
            if block_comment:
                if char == "*" and following == "/":
                    block_comment = False
                    index += 2
                else:
                    index += 1
                continue
            if quote:
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == quote:
                    quote = None
                index += 1
                continue
            if char in ("'", '"', "`"):
                quote = char
            elif char == "/" and following == "/":
                line_comment = True
                index += 1
            elif char == "/" and following == "*":
                block_comment = True
                index += 1
            elif char == "(":
                paren_depth += 1
            elif char == ")":
                paren_depth -= 1
                if paren_depth == 0:
                    parts.append(source[part_start:index].strip())
                    calls.append((call_start, parts))
                    index += 1
                    break
            elif char == "{":
                brace_depth += 1
            elif char == "}":
                brace_depth = max(0, brace_depth - 1)
            elif char == "[":
                bracket_depth += 1
            elif char == "]":
                bracket_depth = max(0, bracket_depth - 1)
            elif (char == "," and paren_depth == 1
                  and brace_depth == 0 and bracket_depth == 0):
                parts.append(source[part_start:index].strip())
                part_start = index + 1
            index += 1
        cursor = max(index, call_start + len(marker))


class WebDomLifecycleContractTests(unittest.TestCase):
    def test_page_templates_are_cached_without_preload_mounts(self):
        self.assertIn("_pageTemplates: {}", PAGE_LOADER)
        self.assertIn("_cachePageTemplates(moduleName, html)", PAGE_LOADER)
        self.assertIn("{ mount: false }", PAGE_LOADER)
        self.assertIn("return options.mount === false ? null : this._mountPage(pageId)", PAGE_LOADER)

    def test_old_page_is_cleaned_and_removed_before_new_mount(self):
        hide = STATE.index("this._firePageLifecycle(this._pageHideHandlers, 'page-hide')")
        remove = STATE.index("PageLoader.unmountPages()", hide)
        mount = STATE.index("PageLoader.mountPage(normalizedPage + '-page')", remove)
        self.assertLess(hide, remove)
        self.assertLess(remove, mount)
        self.assertIn("container.querySelectorAll(':scope > .page')", PAGE_LOADER)

    def test_auth_boot_page_cannot_mount_after_newer_navigation(self):
        boot_start = STATE_SESSION.index("AppState._bootDashboardAfterAuth = async function()")
        boot_end = STATE_SESSION.index("AppState._showAppPage = async function", boot_start)
        boot = STATE_SESSION[boot_start:boot_end]
        show_start = boot_end
        show_end = STATE_SESSION.index("AppState._loginErrorText", show_start)
        show = STATE_SESSION[show_start:show_end]

        self.assertIn("var bootNavSeq = this._pageNavSeq || 0", boot)
        self.assertIn("await this._showAppPage(bootNavSeq)", boot)
        self.assertIn("bootNavSeq === (this._pageNavSeq || 0)", boot)
        self.assertIn("this.currentPage === 'dashboard'", boot)
        self.assertIn("await this.loadPage('dashboard-page', { mount: false })", show)
        stale = show.index("if (appNavSeq !== (this._pageNavSeq || 0)) return false")
        mount = show.index("PageLoader.mountPage('dashboard-page')", stale)
        self.assertLess(stale, mount)
        self.assertIn("PageLoader.unmountPages()", show[stale:mount])

    def test_remounted_pages_can_bind_events_again(self):
        for flag in (
            "_periphEventsBound",
            "_peEventsBound",
            "_protocolEventsBound",
            "_logsEventsBound",
            "_ruleScriptEventsBound",
            "_featuresEventsBound",
        ):
            self.assertIn(flag, STATE)
        self.assertIn("this._preparePageMount(normalizedPage)", STATE)
        self.assertIn("const moduleWasLoaded", STATE)
        self.assertIn("if (moduleWasLoaded) this._rebindMountedPageEvents(normalizedPage)", STATE)
        for method in (
            "setupDashboardEvents",
            "setupNetworkEvents",
            "setupDeviceConfigEvents",
            "setupPeripheralsEvents",
            "setupPeriphExecEvents",
            "setupUsersEvents",
        ):
            self.assertIn(method, STATE)

    def test_page_actions_wait_for_runtime_module_binding(self):
        busy = STATE.index("target.setAttribute('inert', '')")
        load = STATE.index("loader();", busy)
        ready = STATE.index("target.removeAttribute('inert')", load)
        self.assertLess(busy, load)
        self.assertLess(load, ready)
        self.assertIn("data-module-loading", STATE)
        self.assertIn("waitForPeripheralPageReady(authPage)", UI_SUITE)
        self.assertIn("state?._periphEventsBound !== true", UI_SUITE)

    def test_disabled_feature_fallback_is_not_left_inert(self):
        navigation = STATE.index("async changePage(page)")
        fallback = STATE.index("if (featureUnavailable) {", navigation)
        navigation_block = STATE[navigation:fallback]
        self.assertIn("Object.keys(this._featureMenuMap || {}).find", navigation_block)
        for feature, page in (
                ("ruleScript", "rule-script"),
                ("deviceControl", "device-control"),
                ("logViewer", "logs"),
                ("userAdmin", "users")):
            self.assertIn(f"{feature}: '{page}'", STATE)
        fallback_end = STATE.index("return;", fallback)
        block = STATE[fallback:fallback_end]
        self.assertIn("target.removeAttribute('inert')", block)
        self.assertIn("target.removeAttribute('aria-busy')", block)
        self.assertIn("target.removeAttribute('data-module-loading')", block)

    def test_wait_for_function_timeout_is_an_options_argument(self):
        browser_root = ROOT / "test/browser"
        paths = list((browser_root / "fixtures").glob("*.ts"))
        paths += list((browser_root / "suites").glob("*.ts"))
        paths += list(browser_root.glob("*.ts"))
        misuses = []
        for path in paths:
            source = path.read_text(encoding="utf-8")
            for offset, arguments in _call_arguments(source, "waitForFunction("):
                if (len(arguments) == 2
                        and re.search(r"\btimeout\s*:", arguments[1])):
                    line = source.count("\n", 0, offset) + 1
                    misuses.append(f"{path.relative_to(ROOT)}:{line}")
        self.assertEqual([], misuses,
                         "waitForFunction timeout options must be the third argument")

    def test_fragment_cache_remounts_removed_fragment_dom(self):
        self.assertIn("_fragmentTemplates: {}", PAGE_LOADER)
        self.assertIn("cachedContainer.innerHTML = this._fragmentTemplates[fragmentName]", PAGE_LOADER)

    def test_modals_are_cached_and_destroyed_instead_of_left_hidden(self):
        self.assertIn("_modalTemplates: {}", PAGE_LOADER)
        self.assertIn("_cacheModalTemplates(html, hostId, requiredIds)", PAGE_LOADER)
        self.assertIn("mountModal(modalId)", PAGE_LOADER)
        self.assertIn("unmountModal(modalId)", PAGE_LOADER)
        self.assertNotIn("document.body.appendChild(wrapper.firstChild)", PAGE_LOADER)
        self.assertIn("PageLoader.unmountModal(el)", (ROOT / "web-src/js/state-ui.js").read_text(encoding="utf-8"))

    def test_first_modal_action_waits_for_shared_lazy_template_fetch(self):
        self.assertIn("async ensureModalAsync(modalId, expectedNavSeq)", STATE)
        self.assertIn("await this._loadModals()", STATE)
        self.assertIn("if (this._loadingPages['modals']) return this._loadingPages['modals']", PAGE_LOADER)
        self.assertIn("Modal response missing required templates", PAGE_LOADER)
        for modal_id in (
            "change-password-modal",
            "peripheral-modal",
        ):
            self.assertIn("'" + modal_id + "'", PAGE_LOADER)
        self.assertIn("throw error;", PAGE_LOADER)
        self.assertIn("AppState._modalsLoaded = false", STATE)
        self.assertIn("self._loadModals().catch(function()", STATE_SESSION)
        self.assertIn("backgroundPreloadAllowance", STATE_SESSION)
        self.assertIn("modal templates deferred on constrained memory target", STATE_SESSION)
        allowance_check = STATE_SESSION.index("allowBackgroundPreload().then(function(allow)")
        modal_preload = STATE_SESSION.index("self._loadModals().catch(function()")
        self.assertLess(allowance_check, modal_preload)
        self.assertNotIn("document.querySelector('.modal') !== null", BASE_FIXTURE)
        self.assertIn("this.ensureModalAsync(modalId)", (ROOT / "web-src/js/state-ui.js").read_text(encoding="utf-8"))
        self.assertIn("var modalNavSeq = expectedNavSeq", STATE)
        self.assertIn("if (modalNavSeq !== (this._pageNavSeq || 0)) return null", STATE)
        self.assertIn("this.ensureModalAsync(modalId, actionNavSeq)", STATE)
        for path in (
            "web-src/modules/runtime/peripherals.js",
            "web-src/modules/runtime/periph-exec.js",
            "web-src/modules/admin/rule-script.js",
            "web-src/modules/admin/users.js",
            "web-src/modules/runtime/network.js",
            "web-src/modules/runtime/device-config-transfer.js",
            "web-src/js/state-session.js",
        ):
            self.assertIn("_ensureModalForAction(", (ROOT / path).read_text(encoding="utf-8"))
        self.assertIn("UI-007C", UI_SUITE)
        self.assertIn("__modalFetchCount", UI_SUITE)
        self.assertIn("new Response(modalHtml", UI_SUITE)

    def test_fragment_modals_use_same_lazy_mount_lifecycle(self):
        self.assertIn("this._cacheModalTemplates(html, containerId)", PAGE_LOADER)
        for modal_id in (
            "task-edit-modal",
            "mapping-modal",
            "modbus-device-edit-modal",
            "modbus-device-ctrl-modal",
        ):
            self.assertIn("ensureModal('" + modal_id + "')", (
                ROOT / "web-src/modules/runtime/protocol/modbus-config.js"
            ).read_text(encoding="utf-8") + (
                ROOT / "web-src/modules/runtime/protocol/modbus-control.js"
            ).read_text(encoding="utf-8"))

    def test_repeated_lifecycle_registration_replaces_old_handler(self):
        self.assertGreaterEqual(STATE.count("filter(h => h.name !== name)"), 2)
        self.assertIn("filter(function(binder) { return binder.name !== name; })", STATE)

    def test_document_scoped_device_control_events_bind_only_once(self):
        self.assertIn("_dcFullscreenEventsBound: false", DEVICE_CONTROL)
        self.assertIn("if (!this._dcFullscreenEventsBound)", DEVICE_CONTROL)
        self.assertIn("this._dcFullscreenEventsBound = true", DEVICE_CONTROL)
        self.assertIn("_dcKeyboardEventsBound: false", DEVICE_CONTROL)
        self.assertIn("if (!this._dcKeyboardEventsBound)", DEVICE_CONTROL)
        self.assertIn("this._dcKeyboardEventsBound = true", DEVICE_CONTROL)
        self.assertEqual(1, DEVICE_CONTROL.count("document.addEventListener('keydown'"))
        self.assertIn("_dcLayoutDocumentEventsBound: false", DEVICE_CONTROL_LAYOUT)
        self.assertIn("if (this._dcLayoutDocumentEventsBound) return", DEVICE_CONTROL_LAYOUT)
        self.assertEqual(1, DEVICE_CONTROL_LAYOUT.count("document.addEventListener('mousemove'"))
        self.assertEqual(1, DEVICE_CONTROL_LAYOUT.count("document.addEventListener('mouseup'"))
        self.assertIn("document.querySelector('.dc-control-flow')", DEVICE_CONTROL_LAYOUT)
        self.assertIn("AppState.onPageHide('device-control-layout'", DEVICE_CONTROL_LAYOUT)
        self.assertIn("this._dcCancelLayoutInteraction()", DEVICE_CONTROL_LAYOUT)

    def test_live_regression_covers_twenty_switches_and_dom_bound(self):
        self.assertIn("UI-025A", UI_SUITE)
        self.assertIn("round < 10", UI_SUITE)
        self.assertIn("directPageRoots", UI_SUITE)
        self.assertIn("hiddenPageRoots", UI_SUITE)
        self.assertIn("nodes - baseline", UI_SUITE)
        self.assertIn("UI-025B", UI_SUITE)
        self.assertIn("__dcDocumentListenerAdds", UI_SUITE)

    def test_mobile_regression_covers_every_visible_menu_and_absolute_dom_bound(self):
        self.assertIn("UI-005E", UI_SUITE)
        self.assertIn("for (const width of [360, 390, 430])", UI_SUITE)
        self.assertIn("for (const menuPage of menuPages)", UI_SUITE)
        self.assertIn("document.documentElement.scrollWidth", UI_SUITE)
        self.assertIn("toBeLessThanOrEqual(1800)", UI_SUITE)

    def test_modal_regression_covers_twenty_open_close_cycles(self):
        self.assertIn("UI-007A", UI_SUITE)
        self.assertIn("round < 20", UI_SUITE)
        self.assertIn("visibleModals", UI_SUITE)
        self.assertIn("toHaveCount(0)", UI_SUITE)
        self.assertIn("body > .modal", UI_SUITE)
        self.assertIn("UI-007B", UI_SUITE)
        self.assertIn("Modbus 分片弹窗按需挂载", UI_SUITE)

    def test_modbus_browser_suites_wait_for_runtime_feature_gate(self):
        self.assertIn("isRuntimeModbusReady(authPage)", UI_SUITE)
        self.assertIn("_featureActive?.modbus === true", UI_SUITE)
        self.assertLess(
            UI_SUITE.index("isRuntimeModbusReady(authPage)"),
            UI_SUITE.index("await modbusTab.click()"),
        )
        self.assertIn("g.AppState._featureActive !== null", MODBUS_SUITE)
        self.assertIn("state?._featureActive?.modbus === true", MODBUS_SUITE)

    def test_periph_exec_create_editor_loads_device_apis_serially(self):
        start = PERIPH_EXEC.index("const loadSeq = ++this._peEditLoadSeq")
        end = PERIPH_EXEC.index("this.showModal(modal);", start)
        create_loader = PERIPH_EXEC[start:end]
        self.assertNotIn("Promise.all", create_loader)
        self.assertIn("ensureCreateActive()", create_loader)
        self.assertIn("_isPeriphExecEditLoadActive(loadSeq, pageGeneration)", create_loader)
        peripherals = create_loader.index("apiGet('/api/peripherals'")
        protocol = create_loader.index("apiGet('/api/protocol/config'")
        rules = create_loader.index("apiGet('/api/periph-exec'")
        self.assertLess(peripherals, protocol)
        self.assertLess(protocol, rules)

    def test_periph_exec_editor_stays_coarse_grained_and_serial(self):
        sequence_start = MODULE_LOADER.index("'periph-exec-editor': [") + len("'periph-exec-editor': [")
        sequence_end = MODULE_LOADER.index("]", sequence_start)
        sequence = MODULE_LOADER[sequence_start:sequence_end]
        self.assertIn("'periph-exec'", sequence)
        self.assertIn("'periph-exec-modbus'", sequence)
        self.assertIn("'periph-exec-form'", sequence)
        self.assertEqual(
            ['periph-exec', 'periph-exec-modbus', 'periph-exec-form', 'periph-exec-templates'],
            re.findall(r"'(periph-exec[^']*)'", sequence),
        )
        self.assertIn("_MAX_CONCURRENT_LOADS: 1", MODULE_LOADER)

    def test_periph_exec_action_panels_are_mounted_on_demand(self):
        self.assertIn("_buildPeriphExecModbusPanel()", PERIPH_EXEC_FORM)
        self.assertIn("_buildPeriphExecSensorPanel(data)", PERIPH_EXEC_SENSOR)
        self.assertIn(
            "_buildPeriphExecEffectsDisplayPanel(actionType, actionValue)",
            PERIPH_EXEC_EFFECTS,
        )
        self.assertIn("this._buildPeriphExecSensorPanel(data || {})", PERIPH_EXEC_EDITOR_PANELS)
        self.assertIn(
            "this._buildPeriphExecEffectsDisplayPanel(actionType, actionValue)",
            PERIPH_EXEC_EDITOR_PANELS,
        )
        self.assertIn("_snapshotPeriphExecLazyPanel(host)", PERIPH_EXEC_EDITOR_PANELS)
        self.assertIn(
            "_restorePeriphExecLazyPanel(host, block._peLazyPanelDrafts[requestedType])",
            PERIPH_EXEC_EDITOR_PANELS,
        )
        self.assertIn("host.innerHTML = '';", PERIPH_EXEC_EDITOR_PANELS)
        self.assertIn("this._renderPeriphExecLazyActionPanel(block, actionType, data, forceMount)", PERIPH_EXEC_FORM)
        self.assertIn("this._mountPeriphExecLazyActionPanel(block, actionType);", PERIPH_EXEC_FORM)
        self.assertIn("EXEC-126: 懒加载动作面板切走再切回保留未保存草稿", (
            ROOT / "test/browser/suites/06-periph-exec.spec.ts"
        ).read_text(encoding="utf-8"))

        create_start = PERIPH_EXEC_FORM.index("_createPeriphExecActionElement(data, index)")
        create_end = PERIPH_EXEC_FORM.index("container.appendChild(div);", create_start)
        base_template = PERIPH_EXEC_FORM[create_start:create_end]
        self.assertIn("pe-action-lazy-host", base_template)
        for deferred_control in (
            "pe-poll-tasks-group",
            "pe-sensor-group",
            "pe-neopixel-preset",
            "pe-action-value-oled",
            "pe-action-value-script",
            "pe-trigger-event-select",
        ):
            self.assertNotIn(deferred_control, base_template)

    def test_direct_modbus_targets_destroy_unrelated_lazy_panels(self):
        self.assertGreaterEqual(
            PERIPH_EXEC_MODBUS.count("this._mountPeriphExecLazyActionPanel(block, -1);"),
            2,
        )


if __name__ == "__main__":
    unittest.main()
