"""Regression contracts for live-device fleet and stability acceptance gates."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
FLEET = (ROOT / "scripts/run-device-fleet-tests.ps1").read_text(encoding="utf-8")
BROWSER = (ROOT / "scripts/run-browser-tests.ps1").read_text(encoding="utf-8")
STABILITY = (ROOT / "scripts/run-stability-tests.ps1").read_text(encoding="utf-8")
STABILITY_CONFIG = (
    ROOT / "test/browser/playwright.stability.config.ts"
).read_text(encoding="utf-8")
STABILITY_SETUP = (
    ROOT / "test/browser/stability-global-setup.ts"
).read_text(encoding="utf-8")


class FleetAcceptanceContractTests(unittest.TestCase):
    def test_fleet_read_only_mode_is_explicit_and_forwarded(self):
        self.assertIn("[switch]$ReadOnly", FLEET)
        self.assertIn(
            '$acceptanceMode = if ($ReadOnly) { "ReadOnly" } else { "Full" }',
            FLEET,
        )
        self.assertIn("AcceptanceMode = $acceptanceMode", FLEET)
        self.assertIn("ReadOnly = [bool]$ReadOnly", FLEET)
        self.assertIn('if ($ReadOnly) {\n        $smokeArgs += "-ReadOnly"', FLEET)
        self.assertIn("验收模式: $acceptanceMode", FLEET)

    def test_fleet_gate_covers_browser_mdns_heap_and_reboots(self):
        defaults = (
            "$MinHeapFreeBytes = 25600",
            "$MinHeapMaxAllocBytes = 8000",
            "$MaxHeapDropBytes = 20000",
            "$MaxHeapMaxAllocDropBytes = 12000",
            "$ActiveMinHeapFreeBytes = 20480",
            "$ActiveMinHeapMaxAllocBytes = 4096",
            "$MemoryCooldownSeconds = 30",
        )
        for default in defaults:
            with self.subTest(default=default):
                self.assertIn(default, FLEET)

        gate_start = FLEET.index("$failedDevices = @($fleetResults | Where-Object")
        gate_end = FLEET.index("$ts = Get-Date", gate_start)
        gate = FLEET[gate_start:gate_end]
        for condition in (
            "$mdnsGate",
            "$browserGate",
            "$_.StaOk",
            "$_.SmokePassed",
            "$_.HeapOk",
            "$_.Rebooted",
        ):
            with self.subTest(condition=condition):
                self.assertIn(condition, gate)
        self.assertIn("(-not $_.MdnsRequired)", gate)
        self.assertIn("(-not $BrowserTests)", gate)

    def test_fleet_records_both_heap_capacity_and_fragmentation(self):
        required_fields = (
            "HeapMaxAllocBefore",
            "HeapMaxAllocAfter",
            "HeapCooldown",
            "HeapMaxAllocCooldown",
            "HeapDropBytes",
            "HeapMaxAllocDropBytes",
            "HeapOk",
        )
        for field in required_fields:
            with self.subTest(field=field):
                self.assertIn(field, FLEET)
        self.assertIn("required DRAM metrics unavailable", FLEET)
        self.assertIn("heap gate failed", FLEET)
        self.assertIn("active heap free", FLEET)
        self.assertIn("cooldown heap free", FLEET)
        self.assertIn("Start-Sleep -Seconds $MemoryCooldownSeconds", FLEET)

    def test_fleet_passes_auth_to_browser_runner(self):
        self.assertRegex(
            FLEET,
            r"Invoke-BrowserSmoke\s+-DeviceIP\s+\$dev\.IP\s+"
            r"-AuthUsername\s+\$Username\s+-AuthPassword\s+\$Password",
        )
        self.assertIn("-Username $AuthUsername -Password $AuthPassword", FLEET)

    def test_monitor_falls_back_to_ip_when_mdns_is_not_configured(self):
        self.assertIn(
            '$monitorHost = if ([string]::IsNullOrWhiteSpace([string]$dev.Domain))',
            FLEET,
        )
        self.assertIn('"http://$monitorHost/api/system/status"', FLEET)


class BrowserAuthenticationContractTests(unittest.TestCase):
    def test_browser_runner_scopes_auth_environment_to_the_process(self):
        self.assertIn("[string]$Username = $env:AUTH_USERNAME", BROWSER)
        self.assertIn("[string]$Password = $env:AUTH_PASSWORD", BROWSER)
        self.assertIn("$env:AUTH_USERNAME = $Username", BROWSER)
        self.assertIn("$env:AUTH_PASSWORD = $Password", BROWSER)
        self.assertIn(
            '[Environment]::SetEnvironmentVariable("AUTH_USERNAME", $originalAuthUsername',
            BROWSER,
        )
        self.assertIn(
            '[Environment]::SetEnvironmentVariable("AUTH_PASSWORD", $originalAuthPassword',
            BROWSER,
        )
        self.assertIsNone(re.search(r"Write-Host[^\r\n]*\$Password", BROWSER))


class StabilityAcceptanceContractTests(unittest.TestCase):
    def test_playwright_report_and_artifacts_use_distinct_paths(self):
        self.assertIn("$env:PLAYWRIGHT_JSON_OUTPUT_NAME = $jsonOut", STABILITY)
        self.assertIn('"--output", $artifactDir', STABILITY)
        self.assertNotIn('$pwArgs += @("--output", $jsonOut)', STABILITY)
        self.assertIn("Remove-Item -LiteralPath $jsonOut", STABILITY)
        self.assertIn("& node $playwrightCli @pwArgs", STABILITY)
        self.assertNotIn("& npx @pwArgs", STABILITY)

    def test_nonzero_cli_and_empty_or_missing_reports_fail(self):
        self.assertIn("$playwrightExit = $LASTEXITCODE", STABILITY)
        self.assertIn("if ($playwrightExit -ne 0)", STABILITY)
        self.assertIn("Playwright JSON report missing", STABILITY)
        self.assertIn("Playwright report contains zero tests", STABILITY)
        self.assertIn("Playwright report contains zero executed tests", STABILITY)
        self.assertIn("$result.Passed = ($issues.Count -eq 0)", STABILITY)
        self.assertIn("$failedAll.Count -gt 0", STABILITY)

    def test_json_statistics_use_test_status_not_result_status(self):
        self.assertIn('$_.status -in @("expected", "flaky")', STABILITY)
        self.assertIn('$_.status -eq "unexpected"', STABILITY)
        self.assertIn('$_.status -eq "skipped"', STABILITY)
        self.assertNotIn(
            '$_.results | Where-Object { $_.status -eq "expected"', STABILITY
        )
        self.assertIn("function Get-SuiteTests", STABILITY)

    def test_stability_defaults_to_zero_tolerated_failures(self):
        self.assertIn("[double]$MaxFailureRatePercent = 0", STABILITY)

    def test_stability_runner_propagates_auth_without_logging_password(self):
        self.assertIn("[string]$Username = $env:AUTH_USERNAME", STABILITY)
        self.assertIn("[string]$Password = $env:AUTH_PASSWORD", STABILITY)
        self.assertIn("$env:AUTH_USERNAME = $Username", STABILITY)
        self.assertIn("$env:AUTH_PASSWORD = $Password", STABILITY)
        self.assertIsNone(re.search(r"Write-Host[^\r\n]*\$Password", STABILITY))

    def test_stability_health_sampling_uses_public_current_status_schema(self):
        self.assertIn('"$BaseUrl/api/system/status"', STABILITY)
        self.assertNotIn('"$BaseUrl/api/system/info?probe=1"', STABILITY)
        self.assertIn("$memory.dram8Free", STABILITY)
        self.assertIn("payload.memory", STABILITY_SETUP)
        self.assertIn("memory.dram8Largest", STABILITY_SETUP)
        self.assertIn("`${BASE_URL}/api/system/status`", STABILITY_SETUP)
        self.assertNotIn("`${BASE_URL}/api/system/info?probe=1`", STABILITY_SETUP)

    def test_stability_rounds_drive_playwright_repeat_each(self):
        self.assertIn("const parsedStabilityRounds = Number.parseInt", STABILITY_CONFIG)
        self.assertIn("repeatEach: stabilityRounds", STABILITY_CONFIG)
        self.assertIn("parsedStabilityRounds > 0", STABILITY_CONFIG)


if __name__ == "__main__":
    unittest.main()
