"""Regression contracts for valid optional-feature responses in live smoke tests."""

import json
import pathlib
import shutil
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SMOKE_SCRIPT = (ROOT / "scripts/smoke-test-device.ps1").read_text(encoding="utf-8")
SOAK_SCRIPT = (ROOT / "scripts/soak-test-device.ps1").read_text(encoding="utf-8")
DEVICE_MATRIX = json.loads(
    (ROOT / "scripts/device-api-test-matrix.json").read_text(encoding="utf-8")
)


class DeviceSmokeSemanticsTests(unittest.TestCase):
    def test_rule_script_empty_array_and_invalid_shapes_in_powershell(self):
        executable = shutil.which("pwsh") or shutil.which("powershell")
        if executable is None:
            self.skipTest("PowerShell unavailable for actual smoke semantics")
        command = r'''
$ErrorActionPreference='Stop'
$tokens=$null;$errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile('scripts/smoke-test-device.ps1',[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'PowerShell AST parse failed'}
foreach($name in @('Get-ObjectValue','Test-ObjectHasProperty','Test-CheckSemantics')){
 $definition=$ast.Find({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name},$true)
 . ([scriptblock]::Create($definition.Extent.Text))
}
$cases=@(
 @{label='empty';json='{"success":true,"data":[]}';expected=$true},
 @{label='one';json='{"success":true,"data":[{"id":"x"}]}';expected=$true},
 @{label='missing';json='{"success":true}';expected=$false},
 @{label='null';json='{"success":true,"data":null}';expected=$false},
 @{label='object';json='{"success":true,"data":{}}';expected=$false},
 @{label='string';json='{"success":true,"data":""}';expected=$false}
)
foreach($case in $cases){
 $result=Test-CheckSemantics -Name 'rule-scripts' -Response ($case.json|ConvertFrom-Json)
 if([string]::IsNullOrEmpty($result) -ne $case.expected){throw ('Contract failed: '+$case.label)}
}
Write-Output 'SIX_RULE_SCRIPT_CONTRACT_CASES_PASS'
'''
        result = subprocess.run([executable, '-NoProfile', '-Command', command], cwd=ROOT,
                                capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('SIX_RULE_SCRIPT_CONTRACT_CASES_PASS', result.stdout)

    def test_read_only_mode_skips_mutating_smoke_checks(self):
        self.assertIn("[switch]$ReadOnly", SMOKE_SCRIPT)
        self.assertIn(
            '$acceptanceMode = if ($ReadOnly) { "ReadOnly" } else { "Full" }',
            SMOKE_SCRIPT,
        )

        ntp_dispatch_start = SMOKE_SCRIPT.index('if ($type -eq "mqtt-ntp-sync")')
        ntp_dispatch = SMOKE_SCRIPT[ntp_dispatch_start : ntp_dispatch_start + 600]
        self.assertIn("if ($ReadOnly)", ntp_dispatch)
        self.assertIn("Add-ReadOnlySkippedCheck", ntp_dispatch)
        self.assertIn("else {\n            Add-MqttNtpSyncCheck", ntp_dispatch)

        import_start = SMOKE_SCRIPT.index(
            "# Test 3: Import config (using exported bundle)."
        )
        import_dispatch = SMOKE_SCRIPT[import_start : import_start + 900]
        self.assertIn("if ($ReadOnly)", import_dispatch)
        self.assertIn("Add-ReadOnlySkippedCheck", import_dispatch)
        self.assertIn("elseif ($exportSingleOk)", import_dispatch)
        self.assertIn("skipped (read-only acceptance mode)", SMOKE_SCRIPT)

    def test_read_only_mode_keeps_safe_get_and_batch_checks(self):
        policy_start = SMOKE_SCRIPT.index("function Test-ReadOnlySafeMatrixCheck")
        policy_end = SMOKE_SCRIPT.index("function Test-ObjectHasProperty", policy_start)
        policy = SMOKE_SCRIPT[policy_start:policy_end]
        self.assertIn('$method.ToUpperInvariant() -eq "GET"', policy)
        self.assertIn('$path -ne "/api/batch"', policy)
        self.assertIn('$subMethod = "GET"', policy)
        self.assertIn('$subMethod.ToUpperInvariant() -ne "GET"', policy)

        loop_start = SMOKE_SCRIPT.index("foreach ($check in Get-MatrixChecks)")
        loop_end = SMOKE_SCRIPT.index("# Configuration transfer end-to-end tests")
        dispatch = SMOKE_SCRIPT[loop_start:loop_end]
        self.assertIn("Test-ReadOnlySafeMatrixCheck -Check $check", dispatch)
        self.assertIn("Add-MultiSessionCheck", dispatch)
        self.assertIn("Add-BearerOverCookieCheck", dispatch)
        self.assertIn("Add-Check", dispatch)

        for check in DEVICE_MATRIX["checks"]:
            if check.get("path") != "/api/batch":
                continue
            with self.subTest(check=check["name"]):
                self.assertTrue(check.get("body", {}).get("requests"))
                self.assertTrue(
                    all(
                        request.get("method", "GET").upper() == "GET"
                        for request in check["body"]["requests"]
                    )
                )

    def test_empty_log_array_is_valid(self):
        start = SMOKE_SCRIPT.index('"logs-list" {')
        end = SMOKE_SCRIPT.index('"logs-tail" {', start)
        block = SMOKE_SCRIPT[start:end]
        self.assertIn(
            'Test-ObjectHasProperty -Object $Response -Name "data"', block
        )
        self.assertNotIn('$null -eq $data', block)

    def test_missing_ntp_publish_topic_is_a_valid_precondition(self):
        start = SMOKE_SCRIPT.index("function Add-MqttNtpSyncCheck")
        block = SMOKE_SCRIPT[start : start + 2400]
        self.assertIn("No enabled NTP_SYNC publish topic", block)
        self.assertIn("MQTT/NTP publish prerequisite is not configured", block)

    def test_raw_config_export_refreshes_an_expired_session(self):
        start = SMOKE_SCRIPT.index("function Invoke-RawWithRetry")
        end = SMOKE_SCRIPT.index("function Invoke-LoginRequest", start)
        block = SMOKE_SCRIPT[start:end]
        self.assertIn("Unauthorized", block)
        self.assertIn("if (Try-Login)", block)

    def test_raw_config_export_decodes_utf8_without_ansi_roundtrip(self):
        start = SMOKE_SCRIPT.index("function Invoke-FastBeeRaw")
        end = SMOKE_SCRIPT.index("function Invoke-RawWithRetry", start)
        block = SMOKE_SCRIPT[start:end]
        self.assertIn("$response.RawContentStream", block)
        self.assertIn("[System.Text.Encoding]::UTF8.GetString", block)
        self.assertIn("$stream.CopyTo($buffer)", block)

    def test_disabled_log_routes_require_exact_status_and_message(self):
        checks = {check["name"]: check for check in DEVICE_MATRIX["checks"]}
        expected_message = "Device logs disabled by runtime feature config"
        for name in ("logs-list", "logs-tail", "logs-info"):
            with self.subTest(name=name):
                check = checks[name]
                self.assertEqual("capability-optional", check["type"])
                self.assertEqual([503], check["allowedStatuses"])
                self.assertEqual([expected_message], check["allowedMessages"])

    def test_smoke_and_soak_require_exact_optional_error_messages(self):
        for script_name, script in (
            ("smoke", SMOKE_SCRIPT),
            ("soak", SOAK_SCRIPT),
        ):
            with self.subTest(script=script_name):
                self.assertIn("[string[]]$AllowedMessages", script)
                self.assertIn("-ccontains $curMessage", script)
                self.assertIn('Properties.Name -contains "error"', script)
                error_value = (
                    "$response.error"
                    if script_name == "smoke"
                    else "$result.Response.error"
                )
                self.assertIn(f"[string]{error_value}", script)
                check_variable = "$check" if script_name == "smoke" else "$endpoint"
                self.assertIn(
                    f'Get-CheckValue -Check {check_variable} -Name "allowedMessages"',
                    script,
                )


if __name__ == "__main__":
    unittest.main()
