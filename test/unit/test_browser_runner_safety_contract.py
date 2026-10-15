"""Guard credentials and load shedding in the browser-test launchers."""

import json
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = (ROOT / "scripts" / "run-browser-tests.ps1").read_text(encoding="utf-8")
BASE_FIXTURE = (ROOT / "test/browser/fixtures/base.fixture.ts").read_text(encoding="utf-8")
DEVICE_CONFIG = (ROOT / "test/browser/playwright.device.config.ts").read_text(encoding="utf-8")
BASE_CONFIG = (ROOT / "test/browser/playwright.config.ts").read_text(encoding="utf-8")
QUICK_CONFIG = (ROOT / "test/browser/playwright.quick.config.ts").read_text(encoding="utf-8")
CONSTRAINED_POLICY = (ROOT / "test/browser/constrained-device-policy.ts").read_text(encoding="utf-8")
ROOT_PACKAGE = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))
BROWSER_PACKAGE = json.loads((ROOT / "test/browser/package.json").read_text(encoding="utf-8"))


class BrowserRunnerSafetyContractTests(unittest.TestCase):
    def test_explicit_read_only_filter_does_not_require_network_credentials(self):
        self.assertIn("$hasExplicitSubset", RUNNER)
        self.assertIn("$writesNetwork = if ($hasExplicitSubset)", RUNNER)
        self.assertIn("$Grep -match '(?i)(NET-|WIFI-|network|wifi)'", RUNNER)

    def test_unfiltered_network_projects_still_require_credentials(self):
        self.assertIn('[string]::IsNullOrWhiteSpace($Project) -or $Project -in @("core", "regression")', RUNNER)
        self.assertIn("Network-writing browser tests require", RUNNER)

    def test_constrained_device_policy_is_serial_fail_fast(self):
        self.assertIn("process.env.FASTBEE_CONSTRAINED_DEVICE = '1'", CONSTRAINED_POLICY)
        self.assertRegex(CONSTRAINED_POLICY, r"workers:\s*1")
        self.assertRegex(CONSTRAINED_POLICY, r"retries:\s*0")
        self.assertRegex(CONSTRAINED_POLICY, r"maxFailures:\s*1")
        self.assertRegex(CONSTRAINED_POLICY, r"fullyParallel:\s*false")

    def test_quick_and_device_configs_share_the_constrained_policy(self):
        for config in (QUICK_CONFIG, DEVICE_CONFIG):
            self.assertIn("constrainedDevicePolicy", config)
            self.assertIn("...constrainedDevicePolicy", config)
        self.assertIn("projects: [smokeProject]", DEVICE_CONFIG)

    def test_device_entry_points_use_fail_fast_config(self):
        self.assertIn('--config=playwright.device.config.ts', RUNNER)
        self.assertEqual(
            ROOT_PACKAGE["scripts"]["test:e2e:device"],
            "node test/browser/node_modules/@playwright/test/cli.js test -c test/browser/playwright.device.config.ts",
        )
        self.assertIn(
            "-c playwright.device.config.ts",
            BROWSER_PACKAGE["scripts"]["test:device"],
        )

    def test_runner_isolates_reports_per_device_and_supports_explicit_output(self):
        self.assertIn('[string]$OutputDir = ""', RUNNER)
        self.assertIn('"artifacts\\browser-tests"', RUNNER)
        self.assertIn('$deviceKey = ConvertTo-SafePathSegment', RUNNER)
        self.assertIn('$runId = "{0}-p{1}"', RUNNER)
        self.assertIn('$env:FASTBEE_PLAYWRIGHT_OUTPUT_DIR = $playwrightOutputDir', RUNNER)
        self.assertIn('$env:FASTBEE_PLAYWRIGHT_REPORT_DIR = $playwrightReportDir', RUNNER)
        self.assertIn('Write-Host "HTML 报告: $htmlReportPath"', RUNNER)
        self.assertIn('Write-Host "JSON 报告: $jsonReportPath"', RUNNER)
        self.assertNotIn('show-report reports\\html', RUNNER)

    def test_playwright_configs_honor_runner_output_without_changing_direct_defaults(self):
        for config in (BASE_CONFIG, DEVICE_CONFIG):
            with self.subTest(config=config[:40]):
                self.assertIn("FASTBEE_PLAYWRIGHT_OUTPUT_DIR", config)
                self.assertIn("FASTBEE_PLAYWRIGHT_REPORT_DIR", config)
                self.assertIn("path.join(externalReportDir, 'html')", config)
                self.assertIn("path.join(externalReportDir, 'results.json')", config)
        self.assertIn("externalOutputDir || '../../artifacts/test-results'", BASE_CONFIG)
        self.assertIn("outputFolder: 'reports/html'", BASE_CONFIG)
        self.assertIn("externalOutputDir || '../../artifacts/test-results/device'", DEVICE_CONFIG)
        self.assertIn("outputFolder: 'reports/device-html'", DEVICE_CONFIG)

    def test_runner_restores_report_environment_after_each_device(self):
        for name in (
            "FASTBEE_PLAYWRIGHT_OUTPUT_DIR",
            "FASTBEE_PLAYWRIGHT_REPORT_DIR",
        ):
            with self.subTest(name=name):
                self.assertIn(
                    f'[Environment]::SetEnvironmentVariable("{name}", $original',
                    RUNNER,
                )

    def test_ui_screenshots_use_shared_evidence_path_helper(self):
        suites = ROOT / "test" / "browser" / "suites"
        for spec in sorted(suites.glob("*.spec.ts")):
            text = spec.read_text(encoding="utf-8")
            with self.subTest(spec=spec.name):
                self.assertNotIn("'test-results/", text)
                self.assertNotIn("`test-results/", text)
        helper = (ROOT / "test" / "browser" / "evidence-paths.ts").read_text(encoding="utf-8")
        self.assertIn("artifacts", helper)
        self.assertIn("test-results", helper)
        self.assertIn("uiScreenshotPath", helper)
        for name in (
            "15-ui-regression.spec.ts",
            "26-device-control-lifecycle.spec.ts",
            "32-periph-exec-remote.spec.ts",
        ):
            with self.subTest(spec=name):
                self.assertIn("uiScreenshotPath", (suites / name).read_text(encoding="utf-8"))

    def test_constrained_fixture_does_not_retry_or_persist_auth_state(self):
        self.assertIn("AUTH_LOGIN_ATTEMPT_LIMIT = CONSTRAINED_DEVICE_MODE ? 1 : 2", BASE_FIXTURE)
        self.assertIn("loginAttempt < AUTH_LOGIN_ATTEMPT_LIMIT", BASE_FIXTURE)
        self.assertNotIn("writeFileSync", BASE_FIXTURE)
        self.assertNotRegex(BASE_FIXTURE, r"storageState\s*:\s*['\"]")


if __name__ == "__main__":
    unittest.main()
