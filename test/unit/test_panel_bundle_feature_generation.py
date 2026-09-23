import pathlib
import shutil
import subprocess
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class PanelBundleFeatureGenerationTests(unittest.TestCase):
    def test_real_feature_generator_handles_display_and_explicit_request(self):
        shell = shutil.which("pwsh") or shutil.which("powershell")
        if not shell:
            self.skipTest("PowerShell is required for deployment feature generation")
        command = r'''
$ErrorActionPreference = 'Stop'
$path = Join-Path (Get-Location) 'scripts/deploy-config-bundle.ps1'
$tokens = $null; $errors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Deployment script parse error' }
foreach ($name in @('Get-PropertyValue','Get-EnabledFeatures')) {
  $fn = $ast.Find({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true)
  if ($null -eq $fn) { throw "Missing function $name" }
  Invoke-Expression $fn.Extent.Text
}
$script:appliedMemoryProfile = 'none'
$docs = @{
  'network.json' = [pscustomobject]@{enableMDNS=$false}
  'peripherals.json' = [pscustomobject]@{peripherals=@([pscustomobject]@{type=56;enabled=$true})}
  'periph_exec.json' = [pscustomobject]@{rules=@()}
  'protocol.json' = [pscustomobject]@{}
}
$result = Get-EnabledFeatures -Docs $docs -Requested @() -Profile none
if (-not $result.document.features.screenPanel.enabled) { throw 'Enabled display did not enable screenPanel' }
$docs['peripherals.json'].peripherals[0].enabled = $false
$result = Get-EnabledFeatures -Docs $docs -Requested @() -Profile none
if ($result.document.features.screenPanel.enabled) { throw 'Disabled display enabled screenPanel unexpectedly' }
$result = Get-EnabledFeatures -Docs $docs -Requested @('screenPanel,i2cSensors') -Profile none
if (-not $result.document.features.screenPanel.enabled -or -not $result.document.features.i2cSensors.enabled) { throw 'Explicit feature request was lost during serialization' }
$roundtrip = $result.document | ConvertTo-Json -Depth 20 | ConvertFrom-Json
if (-not $roundtrip.features.screenPanel.enabled) { throw 'Serialized feature missing' }
Write-Output 'Screen panel bundle features: 4 cases passed'
'''
        result = subprocess.run([shell, "-NoProfile", "-NonInteractive", "-Command", command],
                                cwd=ROOT, capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(0, result.returncode, result.stdout + result.stderr)
        self.assertIn("4 cases passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
