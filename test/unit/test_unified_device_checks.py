"""Execute device-test selection and threshold policy without connecting to hardware."""
import json
import pathlib
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


@unittest.skipUnless(shutil.which('powershell') or shutil.which('pwsh'), 'PowerShell required')
class UnifiedDeviceChecks(unittest.TestCase):
    def run_ps(self, code):
        proc = subprocess.run(
            [shutil.which('powershell') or shutil.which('pwsh'), '-NoProfile', '-Command', code],
            cwd=ROOT, text=True, capture_output=True, encoding='utf-8', timeout=30,
        )
        self.assertEqual(0, proc.returncode, proc.stderr + proc.stdout)
        return json.loads(proc.stdout)

    def test_capability_selection_preserves_unavailable_checks(self):
        result = self.run_ps(r'''
$ErrorActionPreference = 'Stop'
. ./scripts/device-capability-checks.ps1
$checks = (Get-Content -Raw -Encoding UTF8 ./scripts/device-api-test-matrix.json | ConvertFrom-Json).checks
$caps = [pscustomobject]@{mqtt=$true;modbus=$true;periphExec=$true;ruleScript=$true;ota=$true;auth=$true;logViewer=$true;fileLogging=$true;webServer=$true}
$enabled = @($checks | Where-Object { Test-MatrixCapability $_ $caps } | ForEach-Object name)
$caps.modbus = $false; $caps.ota = $false; $caps.ruleScript = $false
$disabled = @($checks | Where-Object { Test-MatrixCapability $_ $caps } | ForEach-Object name)
$valid = Test-DeviceCapabilities $caps
$caps.modbus = 'false'
$invalid = Test-DeviceCapabilities $caps
$missingRejected = $false
try { Test-MatrixCapability ($checks | Where-Object name -eq 'modbus-status') $null } catch { $missingRejected = $true }
@{enabled=$enabled;disabled=$disabled;valid=$valid;invalid=$invalid;missingRejected=$missingRejected} | ConvertTo-Json -Compress
''')
        self.assertEqual('', result['valid'])
        self.assertIn('must be boolean', result['invalid'])
        self.assertTrue(result['missingRejected'])
        for positive, negative in [('modbus-status', 'modbus-unavailable'),
                                   ('ota-status', 'ota-unavailable'),
                                   ('rule-scripts', 'rule-scripts-unavailable')]:
            self.assertIn(positive, result['enabled'])
            self.assertNotIn(negative, result['enabled'])
            self.assertNotIn(positive, result['disabled'])
            self.assertIn(negative, result['disabled'])
        self.assertIn('system-status', result['disabled'])
        self.assertIn('peripherals', result['disabled'])

    def test_hardware_thresholds_preserve_manual_overrides(self):
        result = self.run_ps(r'''
$ErrorActionPreference = 'Stop'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $PWD 'scripts/soak-test-device.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$ast.FindAll({param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst]}, $false) | ForEach-Object { Invoke-Expression ($_.Extent.Text.Replace('$PSScriptRoot', '(Join-Path $PWD ''scripts'')')) }
$script:RootBoundParameters = @{}
$script:StabilityThresholdSources = @{}
$StabilityPreset = 'release'; $script:HasPsram = $false
Apply-StabilityPreset
$base = Get-StabilityThresholdSnapshot
$script:HasPsram = $true
Apply-StabilityPreset
$psram = Get-StabilityThresholdSnapshot
$script:RootBoundParameters = @{MinHeapMaxAllocBytes=16000}
$script:MinHeapMaxAllocBytes = 16000
Apply-StabilityPreset
@{base=$base;psram=$psram;manual=(Get-StabilityThresholdSnapshot)} | ConvertTo-Json -Compress
''')
        self.assertEqual(10000, result['base']['MinHeapMaxAllocBytes'])
        self.assertEqual(0, result['base']['MinPsramFreeBytes'])
        self.assertEqual(12000, result['psram']['MinHeapMaxAllocBytes'])
        self.assertEqual(262144, result['psram']['MinPsramFreeBytes'])
        self.assertEqual(16000, result['manual']['MinHeapMaxAllocBytes'])
        for case in ('base', 'psram', 'manual'):
            self.assertEqual(0, result[case]['MaxFailureRatePercent'])
            self.assertEqual(0, result[case]['MaxUptimeResetCount'])


if __name__ == '__main__':
    unittest.main()
