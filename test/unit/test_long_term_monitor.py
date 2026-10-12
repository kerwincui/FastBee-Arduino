import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MONITOR = ROOT / "scripts" / "long-term-device-monitor.ps1"
BROWSER_MONITOR = ROOT / "test" / "browser" / "fleet-stability-monitor.mjs"
DEVICE_HEALTH = ROOT / "scripts" / "test-device-health.js"
SCRIPTS_README = ROOT / "scripts" / "README.md"
FLEET_EXAMPLE = ROOT / "scripts" / "device-fleet.example.json"
CORE_MENUS = [
    "dashboard",
    "device",
    "network",
    "peripheral",
    "periph-exec",
    "protocol",
    "features",
]
OPTIONAL_MENUS = ["device-control", "rule-script", "logs", "users"]
ALL_MENUS = CORE_MENUS[:-1] + OPTIONAL_MENUS + ["features"]


class LongTermMonitorScopeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = MONITOR.read_text(encoding="utf-8-sig")
        cls.browser_source = BROWSER_MONITOR.read_text(encoding="utf-8-sig")
        cls.health_source = DEVICE_HEALTH.read_text(encoding="utf-8-sig")
        cls.readme_source = SCRIPTS_README.read_text(encoding="utf-8-sig")

    def _run_menu_report(
        self,
        temp,
        feature_states,
        visible_menus,
        *,
        expected_menus=None,
        no_memory_before=None,
        no_memory_after=None,
        include_mem_after=True,
        append_missing_final_cycle=False,
        blocked_mutations=None,
        style_failures=None,
        legacy_round_fields=False,
    ):
        config_path = temp / "fleet-live.json"
        device_config = {
            "label": "menu-device",
            "ip": "192.0.2.15",
            "profile": "unified",
            "domain": "",
            "serial": "COM15",
        }
        if expected_menus is not None:
            device_config["expectedMenus"] = expected_menus
        config_path.write_text(
            json.dumps([device_config]),
            encoding="utf-8",
        )
        output_dir = temp / "report"
        device_dir = output_dir / "menu-device"
        device_dir.mkdir(parents=True)
        memory = {
            "freeHeap": 50_000,
            "maxAlloc": 30_000,
            "uptime": 100,
            "status503": False,
        }
        if feature_states is not None:
            memory["featureStates"] = feature_states
        if no_memory_before is not None:
            memory["noMemoryFeatures"] = no_memory_before
        memory_after = dict(memory)
        if no_memory_after is not None:
            memory_after["noMemoryFeatures"] = no_memory_after
        memory_cooldown = dict(memory_after)
        cycle = {
            "t": "2026-09-16T00:00:00.000Z",
            "cycle": 1,
            "device": "menu-device",
            "probe": {"ok": True, "rtt": 10, "attempts": 1},
            "round": {
                "login": {"ok": True, "ms": 20, "error": None},
                "visibleMenus": visible_menus,
                "menus": [
                    {"page": page, "ok": True, "ms": 10, "error": None}
                    for page in visible_menus
                ],
                "consoleErrors": [],
                "pageErrors": [],
                "httpErrors": [],
                "webguard503": 0,
                "domErrorToasts": 0,
                "navigationRetries": 0,
                "interactionRetries": 0,
                "ignoredSession401": 0,
                "ignoredNavigationAborts": 0,
                "blockedMutations": blocked_mutations or [],
                "readOnlyPosts": {"batchGet": 0, "motorStatus": 0},
                "mainShellStyle": {"ok": not bool(style_failures)},
                "mainShellStyleFailures": style_failures or [],
            },
            "mem": memory,
            "reboot": False,
            "browserRetries": 0,
            "error": None,
        }
        if include_mem_after:
            cycle["memAfter"] = memory_after
            cycle["memCooldown"] = memory_cooldown
        if legacy_round_fields:
            for field in (
                "blockedMutations",
                "readOnlyPosts",
                "mainShellStyle",
                "mainShellStyleFailures",
            ):
                cycle["round"].pop(field, None)
        cycles = [cycle]
        if append_missing_final_cycle:
            final_cycle = json.loads(json.dumps(cycle))
            final_cycle["t"] = "2026-09-16T00:01:00.000Z"
            final_cycle["cycle"] = 2
            final_cycle["mem"].pop("noMemoryFeatures", None)
            if "memAfter" in final_cycle:
                final_cycle["memAfter"].pop("noMemoryFeatures", None)
            if "memCooldown" in final_cycle:
                final_cycle["memCooldown"].pop("noMemoryFeatures", None)
            cycles.append(final_cycle)
        (device_dir / "cycles.jsonl").write_text(
            "".join(json.dumps(item) + "\n" for item in cycles), encoding="utf-8"
        )
        result = subprocess.run(
            [
                "node",
                str(BROWSER_MONITOR),
                "--report-only",
                "--device-config",
                str(config_path),
                "--output-dir",
                str(output_dir),
            ],
            cwd=ROOT,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(
            (output_dir / "fleet-stability-verdict.json").read_text(encoding="utf-8")
        )

    def test_device_catalog_uses_unique_ports_and_default_scope_is_four_requested_devices(self):
        for port in ("COM4", "COM11", "COM15", "COM7", "COM16"):
            self.assertIn(f'Port = "{port}"', self.source)
        self.assertIn('[string[]]$Ports = @("COM4", "COM11", "COM15", "COM16")', self.source)
        self.assertIn('$devices = @($deviceCatalog | Where-Object', self.source)

    def test_monitor_discovers_post_deployment_sta_address_from_serial(self):
        self.assertIn('$device.Url = "http://$([string]$result.telemetry.ip)"', self.source)
        self.assertIn('$normalizedPorts', self.source)
        self.assertIn('Unknown device port(s)', self.source)

    def test_browser_long_run_covers_same_five_devices(self):
        expected = {
            "esp32c3-f4r0": "192.168.5.55",
            "esp32c6-f4r0": "192.168.5.19",
            "esp32-f4r0": "192.168.5.21",
            "esp32s3-f16r8": "192.168.5.22",
            "esp32s3-f8r0": "192.168.5.67",
        }
        for label, ip in expected.items():
            self.assertIn(f"label: '{label}', host: '{ip}'", self.browser_source)

    def test_browser_long_run_still_exercises_constrained_devices(self):
        self.assertIn("const HARD_SKIP_HEAP = RISK_GUARD_TRIGGER", self.browser_source)
        self.assertIn("const HARD_SKIP_MAX_ALLOC = 4096", self.browser_source)
        self.assertIn("const GENTLE_CYCLE_HEAP = 45000", self.browser_source)
        self.assertIn("const attemptRound = await browserRound(", self.browser_source)
        self.assertIn("gentle ? MENU_DELAY_GENTLE : MENU_DELAY_NORMAL", self.browser_source)
        self.assertNotIn("heapNow < LIGHT_CYCLE_HEAP", self.browser_source)

    def test_browser_long_run_accepts_isolated_output_directory(self):
        self.assertIn("else if (a === '--output-dir')", self.browser_source)
        self.assertIn("const OUT_DIR = OPTS.outputDir", self.browser_source)
        self.assertIn("assertFreshOutput()", self.browser_source)
        self.assertIn("--allow-existing-output", self.browser_source)

    def test_browser_monitor_help_documents_live_device_config(self):
        result = subprocess.run(
            ["node", str(BROWSER_MONITOR), "--help"],
            cwd=ROOT,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--device-config <json>", result.stdout)
        self.assertIn("label/ip/profile/domain/serial", result.stdout)
        self.assertIn("optional expectedMenus", result.stdout)
        self.assertIn("--viewport <width>x<height>", result.stdout)
        self.assertIn("default 1366x800", result.stdout)

    def test_browser_monitor_accepts_and_validates_viewport(self):
        self.assertIn("viewport: OPTS.viewport", self.browser_source)
        self.assertIn("viewport: { width: 1366, height: 800 }", self.browser_source)
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            for index, viewport in enumerate(("1440x900", "390x844")):
                result = subprocess.run(
                    [
                        "node",
                        str(BROWSER_MONITOR),
                        "--report-only",
                        "--viewport",
                        viewport,
                        "--output-dir",
                        str(temp / f"valid-{index}"),
                    ],
                    cwd=ROOT,
                    text=True,
                    encoding="utf-8",
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

            for index, viewport in enumerate(("390", "319x844", "390x2161")):
                result = subprocess.run(
                    ["node", str(BROWSER_MONITOR), "--report-only", "--viewport", viewport],
                    cwd=ROOT,
                    text=True,
                    encoding="utf-8",
                    capture_output=True,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0, viewport)
                self.assertIn("--viewport", result.stderr)

    def test_browser_monitor_loads_and_filters_live_device_config(self):
        fleet = [
            {
                "label": "esp32s3-F16R8",
                "ip": "192.0.2.15",
                "profile": "unified",
                "domain": "fastbee-s3.local",
                "serial": "COM15",
            },
            {
                "label": "esp32c3-F4R0",
                "ip": "192.0.2.4",
                "profile": "unified",
                "domain": "",
                "serial": "COM4",
            },
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            config_path = temp / "fleet-live.json"
            # Windows PowerShell 5.1 writes UTF-8 with a BOM; the CLI must
            # accept that common deployment-report format.
            config_path.write_text("\ufeff" + json.dumps(fleet), encoding="utf-8")
            output_dir = temp / "report"
            result = subprocess.run(
                [
                    "node",
                    str(BROWSER_MONITOR),
                    "--report-only",
                    "--device-config",
                    str(config_path),
                    "--devices",
                    "esp32s3-F16R8",
                    "--output-dir",
                    str(output_dir),
                ],
                cwd=ROOT,
                text=True,
                encoding="utf-8",
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            verdict = json.loads(
                (output_dir / "fleet-stability-verdict.json").read_text(encoding="utf-8")
            )
            self.assertEqual(verdict["devices"], ["esp32s3-F16R8"])
            report = (output_dir / "fleet-stability-report.md").read_text(encoding="utf-8")
            self.assertIn("esp32s3-F16R8", report)
            self.assertNotIn("esp32c3-F4R0", report)

    def test_browser_monitor_rejects_unknown_or_duplicate_device_filters(self):
        fleet = [
            {
                "label": "esp32s3-F16R8",
                "ip": "192.0.2.15",
                "profile": "unified",
                "domain": "",
                "serial": "COM15",
            },
            {
                "label": "esp32c3-F4R0",
                "ip": "192.0.2.4",
                "profile": "unified",
                "domain": "",
                "serial": "COM4",
            },
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            config_path = temp / "fleet-live.json"
            config_path.write_text(json.dumps(fleet), encoding="utf-8")
            cases = [
                ("esp32s3-F16R8,missing-device", "unknown label(s): missing-device"),
                ("esp32s3-F16R8,esp32s3-F16R8", "duplicate label(s): esp32s3-F16R8"),
            ]
            for index, (labels, expected_error) in enumerate(cases):
                with self.subTest(labels=labels):
                    result = subprocess.run(
                        [
                            "node",
                            str(BROWSER_MONITOR),
                            "--report-only",
                            "--device-config",
                            str(config_path),
                            "--devices",
                            labels,
                            "--output-dir",
                            str(temp / f"invalid-filter-{index}"),
                        ],
                        cwd=ROOT,
                        text=True,
                        encoding="utf-8",
                        capture_output=True,
                        check=False,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(expected_error, result.stderr)

    def test_fleet_example_does_not_hardcode_menu_intent_by_hardware_model(self):
        fleet = json.loads(FLEET_EXAMPLE.read_text(encoding="utf-8"))
        self.assertTrue(fleet)
        for device in fleet:
            self.assertNotIn("expectedMenus", device)

    def test_browser_monitor_menu_gate_rejects_missing_and_unexpected_pages(self):
        inactive = {
            "deviceControl": "disabled",
            "ruleScript": "disabled",
            "logViewer": "disabled",
            "userAdmin": "disabled",
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            verdict = self._run_menu_report(
                Path(temp_dir), inactive, CORE_MENUS[:-1]
            )
            self.assertFalse(verdict["ok"])
            self.assertTrue(
                any("菜单集合不匹配" in reason and "features" in reason for reason in verdict["reasons"])
            )
            self.assertTrue(any("期望菜单未成功" in reason for reason in verdict["reasons"]))

        with tempfile.TemporaryDirectory() as temp_dir:
            verdict = self._run_menu_report(
                Path(temp_dir), inactive, CORE_MENUS + ["logs"]
            )
            self.assertFalse(verdict["ok"])
            self.assertTrue(
                any("菜单集合不匹配" in reason and "logs" in reason for reason in verdict["reasons"])
            )

    def test_browser_monitor_menu_gate_accepts_different_feature_sets(self):
        feature_sets = [
            (
                {
                    "deviceControl": "disabled",
                    "ruleScript": "disabled",
                    "logViewer": "disabled",
                    "userAdmin": "disabled",
                },
                CORE_MENUS,
            ),
            (
                {
                    "deviceControl": "active",
                    "ruleScript": "active",
                    "logViewer": "active",
                    "userAdmin": "active",
                },
                CORE_MENUS[:-1] + OPTIONAL_MENUS + ["features"],
            ),
        ]
        for feature_states, visible_menus in feature_sets:
            with self.subTest(feature_states=feature_states), tempfile.TemporaryDirectory() as temp_dir:
                verdict = self._run_menu_report(
                    Path(temp_dir), feature_states, visible_menus
                )
                self.assertTrue(verdict["ok"], verdict["reasons"])

    def test_browser_monitor_explicit_menu_contract_rejects_runtime_degradation(self):
        inactive = {
            "deviceControl": "no-memory",
            "ruleScript": "no-memory",
            "logViewer": "no-memory",
            "userAdmin": "no-memory",
        }
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            verdict = self._run_menu_report(
                temp,
                inactive,
                CORE_MENUS,
                expected_menus=ALL_MENUS,
            )
            self.assertFalse(verdict["ok"])
            mismatch = next(
                reason for reason in verdict["reasons"] if "菜单集合不匹配" in reason
            )
            for menu in OPTIONAL_MENUS:
                self.assertIn(menu, mismatch)
            report = (temp / "report" / "fleet-stability-report.md").read_text(
                encoding="utf-8"
            )
            self.assertIn("总体期望菜单通过率：**63.6%**（7/11）", report)
        self.assertIn(
            "const expectedMenus = Array.isArray(dev.expectedMenus)",
            self.browser_source,
        )
        self.assertIn(
            ": expectedMenusFromFeatureStates(rec.mem?.featureStates)",
            self.browser_source,
        )
        self.assertIn(
            "expectedMenus: Array.isArray(d.expectedMenus) ? d.expectedMenus : null",
            self.browser_source,
        )

    def test_screen_panel_menu_is_required_only_when_enabled(self):
        for state, visible, passed in [
            ("active", CORE_MENUS + ["screen-panel"], True),
            ("active", CORE_MENUS, False),
            ("disabled", CORE_MENUS, True),
            ("disabled", CORE_MENUS + ["screen-panel"], False),
        ]:
            with self.subTest(state=state, visible=visible), tempfile.TemporaryDirectory() as folder:
                verdict = self._run_menu_report(Path(folder), {"screenPanel": state}, visible)
                self.assertEqual(verdict["ok"], passed, verdict["reasons"])

    def test_explicit_screen_menu_contract_survives_runtime_degradation(self):
        expected = CORE_MENUS + ["screen-panel"]
        for visible, passed in [(expected, True), (CORE_MENUS, False)]:
            with self.subTest(visible=visible), tempfile.TemporaryDirectory() as folder:
                verdict = self._run_menu_report(
                    Path(folder), {"screenPanel": "disabled"}, visible,
                    expected_menus=expected,
                )
                self.assertEqual(verdict["ok"], passed, verdict["reasons"])

    def test_browser_monitor_rejects_invalid_expected_menu_contracts(self):
        invalid_contracts = [
            ([], "expectedMenus must be a non-empty array"),
            ("dashboard", "expectedMenus must be a non-empty array"),
            (["dashboard", 42], "expectedMenus must contain only non-empty strings"),
            (["dashboard", "not-a-menu"], "expectedMenus contains unknown menu 'not-a-menu'"),
            (["dashboard", "dashboard"], "expectedMenus contains duplicate menu 'dashboard'"),
        ]
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            for index, (expected_menus, expected_error) in enumerate(invalid_contracts):
                with self.subTest(expected_menus=expected_menus):
                    config_path = temp / f"invalid-menus-{index}.json"
                    config_path.write_text(
                        json.dumps(
                            [
                                {
                                    "label": f"menu-device-{index}",
                                    "ip": f"192.0.2.{index + 1}",
                                    "profile": "unified",
                                    "domain": "",
                                    "serial": f"COM{index + 1}",
                                    "expectedMenus": expected_menus,
                                }
                            ]
                        ),
                        encoding="utf-8",
                    )
                    result = subprocess.run(
                        [
                            "node",
                            str(BROWSER_MONITOR),
                            "--report-only",
                            "--device-config",
                            str(config_path),
                            "--output-dir",
                            str(temp / f"invalid-menu-report-{index}"),
                        ],
                        cwd=ROOT,
                        text=True,
                        encoding="utf-8",
                        capture_output=True,
                        check=False,
                    )
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn(expected_error, result.stderr)

    def test_browser_report_shows_no_memory_recovery_before_and_after_round(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            verdict = self._run_menu_report(
                temp,
                None,
                ["dashboard", "device"],
                no_memory_before=4,
                no_memory_after=0,
            )
            self.assertFalse(verdict["ok"])
            self.assertTrue(
                any("no-memory 峰值 4" in reason for reason in verdict["reasons"])
            )
            report = (temp / "report" / "fleet-stability-report.md").read_text(
                encoding="utf-8"
            )
            self.assertIn("周期前最新 **4**（峰值 4）", report)
            self.assertIn("周期后最终 **0**（峰值 0）", report)
            self.assertIn("4 → 0（已恢复）", report)

    def test_browser_report_keeps_legacy_cycles_without_after_sample_readable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            verdict = self._run_menu_report(
                temp,
                None,
                ["dashboard", "device"],
                no_memory_before=3,
                include_mem_after=False,
            )
            self.assertFalse(verdict["ok"])
            self.assertTrue(
                any("no-memory 峰值 3" in reason for reason in verdict["reasons"])
            )
            report = (temp / "report" / "fleet-stability-report.md").read_text(
                encoding="utf-8"
            )
            self.assertIn("周期前最新 **3**（峰值 3）", report)
            self.assertIn("周期后最终 无采样", report)
            self.assertNotIn("最新成对采样", report)

    def test_browser_report_does_not_use_stale_no_memory_samples_as_latest(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            verdict = self._run_menu_report(
                temp,
                None,
                ["dashboard", "device"],
                no_memory_before=3,
                no_memory_after=1,
                append_missing_final_cycle=True,
            )
            self.assertFalse(verdict["ok"])
            report = (temp / "report" / "fleet-stability-report.md").read_text(
                encoding="utf-8"
            )
            self.assertIn("周期前最新 无采样（历史峰值 3）", report)
            self.assertIn("周期后最终 无采样（历史峰值 1）", report)
            self.assertNotIn("最新成对采样", report)

    def test_browser_monitor_keeps_legacy_menu_reports_readable(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            verdict = self._run_menu_report(
                Path(temp_dir),
                None,
                ["dashboard", "device"],
                legacy_round_fields=True,
            )
            self.assertTrue(verdict["ok"], verdict["reasons"])

    def test_browser_monitor_rejects_invalid_hostname_and_empty_output_slug(self):
        invalid_entries = [
            {
                "label": "valid-label",
                "ip": "192.0.2.15",
                "profile": "unified",
                "domain": "bad host.local",
                "serial": "COM15",
            },
            {
                "label": "---",
                "ip": "192.0.2.16",
                "profile": "unified",
                "domain": "fastbee.local",
                "serial": "COM16",
            },
        ]
        expected_errors = ["domain must be a valid hostname", "label must contain an ASCII letter or digit"]
        with tempfile.TemporaryDirectory() as temp_dir:
            temp = Path(temp_dir)
            for index, (entry, expected_error) in enumerate(zip(invalid_entries, expected_errors)):
                config_path = temp / f"invalid-{index}.json"
                config_path.write_text(json.dumps([entry]), encoding="utf-8")
                result = subprocess.run(
                    [
                        "node",
                        str(BROWSER_MONITOR),
                        "--report-only",
                        "--device-config",
                        str(config_path),
                        "--output-dir",
                        str(temp / f"report-{index}"),
                    ],
                    cwd=ROOT,
                    text=True,
                    encoding="utf-8",
                    capture_output=True,
                    check=False,
                )
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected_error, result.stderr)

    def test_browser_monitor_does_not_mislabel_ip_as_mdns(self):
        self.assertIn("const hostVia =", self.browser_source)
        self.assertIn("? 'ip' : 'mdns'", self.browser_source)
        self.assertIn("new Map([", self.browser_source)

    def test_browser_monitor_credentials_can_be_overridden(self):
        self.assertIn("process.env.FASTBEE_WEB_USERNAME", self.browser_source)
        self.assertIn("process.env.FASTBEE_WEB_PASSWORD", self.browser_source)

    def test_browser_monitor_waits_for_cookie_session_restoration(self):
        probe = self.browser_source.index("const sessionProbePromise = page.waitForResponse")
        navigation = self.browser_source.index("await page.goto(`${base}/`", probe)
        decision = self.browser_source.index("const needLogin = sessionValid ? false", navigation)
        self.assertLess(probe, navigation)
        self.assertLess(navigation, decision)
        self.assertIn("payload?.data?.sessionValid", self.browser_source)
        self.assertIn("await sessionProbePromise", self.browser_source)

    def test_browser_monitor_revokes_its_session_when_context_closes(self):
        self.assertIn("const ensureContext = async (base)", self.browser_source)
        self.assertIn("contextBase = base", self.browser_source)
        self.assertIn("`${logoutBase}/api/auth/logout`", self.browser_source)
        self.assertIn("await closingContext.close()", self.browser_source)

    def test_browser_report_describes_the_selected_scope(self):
        self.assertIn("FastBee 舰队 Web 稳定性测试报告", self.browser_source)
        self.assertIn("所选设备的在线、登录、全部可见菜单", self.browser_source)
        self.assertNotIn("五台设备在线、登录、全部可见菜单", self.browser_source)

    def test_browser_monitor_records_4xx_and_clears_watchdog(self):
        self.assertIn("resp.status() >= 400", self.browser_source)
        self.assertIn("if (closing) return", self.browser_source)
        self.assertIn("if (watchdog) clearTimeout(watchdog)", self.browser_source)
        self.assertIn("await runOnce.catch(() => {})", self.browser_source)

    def test_browser_monitor_blocks_unknown_writes_and_requires_main_styles(self):
        for contract in (
            "isReadOnlyBatch",
            "isReadOnlyMotorStatus",
            "await page.route('**/*'",
            "route.abort('blockedbyclient')",
            "serviceWorkers: 'block'",
            "waitForMainShellStyles(page)",
            "blockedMutations",
            "mainShellStyleFailures",
        ):
            with self.subTest(contract=contract):
                self.assertIn(contract, self.browser_source)

        style_check = self.browser_source.index("const style = await waitForMainShellStyles(page)")
        login_success = self.browser_source.index("round.login.ok = true", style_check)
        self.assertLess(style_check, login_success)

    def test_browser_report_rejects_blocked_write_and_missing_main_style(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            verdict = self._run_menu_report(
                Path(temp_dir),
                {"deviceControl": "disabled", "ruleScript": "disabled", "logViewer": "disabled", "userAdmin": "disabled"},
                CORE_MENUS,
                blocked_mutations=[{"method": "POST", "route": "/api/modbus/motor/control", "scope": "device"}],
                style_failures=[{"issues": ["main CSS rule count too small: 0"], "ruleCount": 0}],
            )
            self.assertFalse(verdict["ok"])
            self.assertTrue(any("阻断未知 API 写请求 1 次" in reason for reason in verdict["reasons"]))
            self.assertTrue(any("主框架样式未生效 1 次" in reason for reason in verdict["reasons"]))

    def test_browser_monitor_ignores_only_expected_browser_noise(self):
        self.assertIn("expectedSessionProbe", self.browser_source)
        self.assertIn("/api/auth/session", self.browser_source)
        self.assertIn("failure === 'net::ERR_ABORTED'", self.browser_source)
        self.assertIn("Date.now() <= navigationAbortUntil", self.browser_source)
        self.assertIn("NAVIGATION_ABORT_WINDOW_MS", self.browser_source)
        self.assertIn("const NAVIGATION_ABORT_WINDOW_MS = 10_000", self.browser_source)
        self.assertIn("const requestGenerations = new WeakMap()", self.browser_source)
        self.assertIn("requestGeneration < pageGeneration", self.browser_source)
        self.assertIn("page.on('request', req => requestGenerations.set(req, pageGeneration))", self.browser_source)
        self.assertIn("['fetch', 'xhr', 'eventsource'].includes(req.resourceType())", self.browser_source)
        self.assertIn("url.startsWith(`${base}/api/`)", self.browser_source)
        self.assertIn("ignoredNavigationAborts", self.browser_source)
        self.assertIn("round.httpErrors.push", self.browser_source)

    def test_browser_monitor_rejects_empty_or_invalid_json_success(self):
        self.assertIn("error: 'empty response body'", self.browser_source)
        self.assertIn("error: 'invalid JSON response'", self.browser_source)
        self.assertIn("{ requireJson: true }", self.browser_source)

    def test_browser_monitor_preserves_transient_failures_and_end_uptime(self):
        self.assertIn("transientFailures.push", self.browser_source)
        self.assertIn("const probeRetries =", self.browser_source)
        self.assertIn("rec.browserRetries++", self.browser_source)
        self.assertIn("observeUptime(memAfter, rec, cycle)", self.browser_source)
        self.assertIn("observeUptime(memCooldown, rec, cycle)", self.browser_source)

    def test_browser_monitor_separates_active_and_cooldown_memory_gates(self):
        self.assertIn("const ACTIVE_MIN_HEAP = RISK_GUARD_TRIGGER", self.browser_source)
        self.assertIn("const ACTIVE_MIN_MAX_ALLOC = HARD_SKIP_MAX_ALLOC", self.browser_source)
        self.assertIn("const MEMORY_COOLDOWN_MS = 30_000", self.browser_source)
        self.assertIn("await dropContext();\n        await sleep(MEMORY_COOLDOWN_MS);", self.browser_source)
        self.assertIn("rec.memCooldown = memCooldown", self.browser_source)
        self.assertIn("Math.min(...s.activeHeaps) < ACTIVE_MIN_HEAP", self.browser_source)
        self.assertIn("Math.min(...s.heaps) < ACCEPT_MIN_HEAP", self.browser_source)

    def test_browser_monitor_rejects_empty_or_invalid_json_payloads(self):
        self.assertIn("error: 'empty response body'", self.browser_source)
        self.assertIn("error: 'invalid JSON response'", self.browser_source)
        self.assertIn("{ requireJson: true }", self.browser_source)
        self.assertIn(
            "error: r.error || (r.status ? `HTTP ${r.status}` : 'request failed')",
            self.browser_source,
        )

    def test_browser_monitor_enforces_duration_and_strict_exit(self):
        self.assertIn("else if (a === '--min-duration-hours')", self.browser_source)
        self.assertIn("s.windowMs < requiredWindowMs", self.browser_source)
        self.assertIn("const workerResults = await Promise.allSettled", self.browser_source)
        self.assertIn("const workerErrors = collectWorkerErrors(workerResults, DEVICES_ACTIVE)", self.browser_source)
        self.assertIn("export function collectWorkerErrors", self.browser_source)
        self.assertIn("export function finalizeMonitorRun", self.browser_source)
        self.assertIn("generateReport(workerErrors)", self.browser_source)
        self.assertIn("const runState = finalizeMonitorRun", self.browser_source)
        self.assertIn("process.exitCode = 2", self.browser_source)
        self.assertIn("fleet-stability-verdict.json", self.browser_source)

    def test_browser_monitor_rejected_worker_injection_forces_failure_exit(self):
        source = r'''
import assert from 'node:assert/strict';
import {collectWorkerErrors, finalizeMonitorRun} from './test/browser/fleet-stability-monitor.mjs';
const errors = collectWorkerErrors([
  {status: 'rejected', reason: new Error('synthetic worker crash')},
  {status: 'fulfilled', value: undefined},
], [{label: 'synthetic-device'}, {label: 'healthy-device'}]);
assert.equal(errors.length, 1);
assert.ok(errors[0].startsWith('synthetic-device: worker '));
assert.deepEqual(JSON.parse(errors[0].slice('synthetic-device: worker '.length)),
  {errorType: 'Error', phase: 'worker', safeCode: 'OBSERVER_ERROR'});
assert.ok(!errors[0].includes('synthetic worker crash'));
assert.deepEqual(finalizeMonitorRun({stopRequested: false, workerErrors: errors}), {finished: false, exitCode: 2});
assert.deepEqual(finalizeMonitorRun({stopRequested: false, workerErrors: []}), {finished: true, exitCode: 0});
console.log('worker failure contract passed');
'''
        result = subprocess.run(
            ["node", "--input-type=module", "--eval", source],
            cwd=ROOT,
            text=True,
            encoding="utf-8",
            capture_output=True,
            check=False,
            timeout=20,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_browser_monitor_requires_core_menus_and_memory_health(self):
        self.assertIn("['dashboard', 'device']", self.browser_source)
        self.assertIn("ACCEPT_MIN_HEAP", self.browser_source)
        self.assertIn("ACCEPT_MIN_MAX_ALLOC", self.browser_source)
        self.assertIn("heapTrendDrop", self.browser_source)

    def test_browser_report_grade_reflects_failed_heap_gate(self):
        grade_start = self.browser_source.index("let grade = 'A（稳定）'")
        grade_end = self.browser_source.index("L.push(`**综合评级", grade_start)
        grade_block = self.browser_source[grade_start:grade_end]
        self.assertIn("Math.min(...s.heaps) < ACCEPT_MIN_HEAP", grade_block)
        heap_gate = grade_block.index("Math.min(...s.heaps) < ACCEPT_MIN_HEAP")
        risk_grade = grade_block.index("grade = 'C（有明显风险）'", heap_gate)
        basic_grade = grade_block.index("grade = 'B（基本稳定，存在间歇性问题）'", risk_grade)
        self.assertLess(heap_gate, risk_grade)
        self.assertLess(risk_grade, basic_grade)

    def test_serial_snapshot_precedes_http_probes(self):
        loop = self.source.index("while (-not $script:durationComplete")
        serial = self.source.index("Invoke-SerialSnapshot -Cycle", loop)
        http = self.source.index("if (-not $NoHttp)", loop)
        self.assertLess(serial, http)

    def test_aux_monitor_is_strictly_read_only(self):
        self.assertIn('$acceptanceMode = "strict-read-only"', self.source)
        self.assertIn('--request "GET"', self.source)
        for forbidden in (
            '--request "POST"',
            '--request "PUT"',
            '--request "DELETE"',
            "/api/auth/login",
            "/api/auth/logout",
            "/api/system/reboot",
            "/api/system/restart",
            "/api/config/transfer/import-chunk",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.source)
        self.assertIn("readOnly = $true", self.source)

    def test_serial_monitor_disables_reset_control_lines(self):
        self.assertIn("'--dtr', '0', '--rts', '0'", self.health_source)

    def test_browser_monitor_is_documented_as_authenticated_not_strict_read_only(self):
        self.assertIn("await page.click('#login-button')", self.browser_source)
        self.assertIn("closingContext.request.post", self.browser_source)
        self.assertIn("不属于零 POST 的严格只读监控", self.readme_source)

    def test_aux_monitor_records_transport_latency_and_device_availability(self):
        for contract in (
            "CurlExit = $curlExit",
            '$script:httpCurl18++',
            '$script:httpCurl28++',
            '$script:http503++',
            "p95Ms = Get-MonitorPercentile",
            "p99Ms = Get-MonitorPercentile",
            "statusCounts = $statusCounts",
            "curlExitCounts = $curlExitCounts",
            'type = "http-outage-start"',
            'type = "http-recovered"',
            "currentConsecutiveFailures",
            "maxConsecutiveFailures",
            "cycleSuccessRatePct",
            "httpByDevice = $httpByDevice",
        ):
            with self.subTest(contract=contract):
                self.assertIn(contract, self.source)

    def test_aux_monitor_can_resume_without_counting_the_gap(self):
        for contract in (
            '[string]$ResumeRunDir = ""',
            'stateVersion = 2',
            'ResumeRunDir does not contain state.json',
            'Resume fleet does not match the selected device labels and ports',
            '$script:elapsedAtSessionStart = $script:elapsedMonitorSeconds',
            '$script:elapsedMonitorSeconds = $script:elapsedAtSessionStart + $sessionSeconds',
            '$script:durationComplete = $script:elapsedMonitorSeconds -ge $script:requiredDurationSeconds',
            'resumeCount = $script:resumeCount',
            'function Restore-ResumeArtifacts',
            '$script:cycleCount = [Math]::Max',
        ):
            with self.subTest(contract=contract):
                self.assertIn(contract, self.source)
        self.assertNotIn("durationHoursActive = ((Get-Date) - $script:startedAt)", self.source)

    def test_ap_scope_is_skipped_without_counting_http_failure(self):
        self.assertIn('if ($mode -eq "AP")', self.source)
        self.assertIn('$device.HttpScope = "ap-not-routed"', self.source)
        self.assertIn('$script:httpSkippedAp++', self.source)
        self.assertIn('Status = -1; Success = $null; Skipped = $true', self.source)
        self.assertIn(
            '-not $_.Skipped -and -not (Test-HttpRowOperational -Row $_)',
            self.source,
        )

    def test_sta_observation_restores_real_http_checks(self):
        self.assertIn('elseif ($mode -eq "STA")', self.source)
        self.assertIn('$device.HttpScope = "sta"', self.source)
        self.assertIn('$device.Url = "http://$([string]$result.telemetry.ip)"', self.source)

    def test_disconnected_scope_skips_stale_ip(self):
        self.assertIn('$device.HttpScope = "unavailable"', self.source)
        self.assertIn('$script:httpSkippedUnavailable++', self.source)
        self.assertIn('Device is not connected to STA; HTTP check skipped', self.source)

    def test_operator_can_skip_known_sta_unavailable_port(self):
        self.assertIn('[string[]]$SkipHttpPorts = @()', self.source)
        self.assertIn('$device.HttpSkip = $true', self.source)
        self.assertIn('if ($device.HttpSkip)', self.source)
        self.assertIn('HTTP check explicitly skipped for this port', self.source)

    def test_ap_skip_metric_is_persisted(self):
        self.assertGreaterEqual(self.source.count("httpSkippedAp = $script:httpSkippedAp"), 2)

    def test_uptime_reset_is_a_stability_issue(self):
        self.assertIn('$script:uptimeResets++', self.source)
        self.assertGreaterEqual(self.source.count("uptimeResets = $script:uptimeResets"), 2)
        self.assertIn('"completed_with_issues"', self.source)

    def test_aux_monitor_rejects_short_or_incomplete_acceptance(self):
        self.assertIn(
            '$script:durationComplete = $script:elapsedMonitorSeconds -ge $script:requiredDurationSeconds',
            self.source,
        )
        self.assertIn('$script:cycleCount -eq 0', self.source)
        self.assertIn('$NoHttp -and $NoSerial', self.source)
        self.assertIn('"incomplete"', self.source)
        self.assertIn('$script:monitorExitCode = 2', self.source)
        self.assertIn('if ($script:monitorExitCode -ne 0) { exit $script:monitorExitCode }', self.source)

    def test_aux_monitor_treats_skips_and_degradation_as_issues(self):
        self.assertIn('$script:httpDegraded -gt 0', self.source)
        self.assertIn('$script:httpSkippedAp -gt 0', self.source)
        self.assertIn('$script:httpSkippedUnavailable -gt 0', self.source)
        self.assertIn('$script:serialUnknown -gt 0', self.source)

    def test_aux_monitor_rejects_serial_capture_or_port_gaps(self):
        self.assertIn('$script:serialCaptureFailures++', self.source)
        self.assertIn('$script:serialMissingPorts++', self.source)
        self.assertIn('type = "serial-port-missing"', self.source)
        self.assertIn('$script:serialCaptureFailures -gt 0', self.source)
        self.assertIn('$script:serialMissingPorts -gt 0', self.source)


if __name__ == "__main__":
    unittest.main()
