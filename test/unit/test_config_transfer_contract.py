"""Regression checks for the live configuration export/import round trip."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SMOKE_SCRIPT = (ROOT / "scripts/smoke-test-device.ps1").read_text(encoding="utf-8")


class ConfigTransferContractTests(unittest.TestCase):
    def test_live_smoke_imports_the_just_exported_document(self):
        export_pos = SMOKE_SCRIPT.index(
            'Invoke-RawWithRetry -Path "/api/config/transfer/export?name=device.json"'
        )
        import_pos = SMOKE_SCRIPT.index(
            'Invoke-FastBeeApi -Method POST -Path "/api/config/transfer/import-chunk"'
        )
        self.assertLess(export_pos, import_pos)
        import_block = SMOKE_SCRIPT[import_pos:import_pos + 1800]
        self.assertIn('name = "device.json"', import_block)
        self.assertIn('chunk = $exportSingleContent', import_block)
        self.assertIn('index = 0', import_block)
        self.assertIn('total = 1', import_block)

    def test_import_success_checks_envelope_and_returned_file_name(self):
        self.assertIn('$importChunk.success -eq $true', SMOKE_SCRIPT)
        self.assertIn('([string]$importName -eq "device.json")', SMOKE_SCRIPT)
        self.assertIn('Name = "config-transfer-import-roundtrip"', SMOKE_SCRIPT)

    def test_successful_import_is_exported_and_compared(self):
        import_pos = SMOKE_SCRIPT.index('# Test 3: Import config (using exported bundle)')
        import_block = SMOKE_SCRIPT[import_pos:import_pos + 5000]
        self.assertIn('$readbackContent = Invoke-RawWithRetry', import_block)
        self.assertIn('$expectedNames = @($expectedObject.PSObject.Properties.Name | Sort-Object)', import_block)
        self.assertIn('Compare-Object -ReferenceObject $expectedNames', import_block)
        self.assertIn('$expectedValue -cne $actualValue', import_block)
        self.assertIn('$nameDifference.Count -eq 0', import_block)
        self.assertIn('$valueDifference.Count -eq 0', import_block)
        self.assertIn('$readbackAttempt -le $RetryCount', import_block)
        self.assertIn('$actualObject.success -eq $false', import_block)
        self.assertIn('Name = "config-transfer-import-readback"', import_block)

    def test_raw_export_is_decoded_as_utf8_before_roundtrip(self):
        raw_pos = SMOKE_SCRIPT.index('function Invoke-FastBeeRaw')
        raw_block = SMOKE_SCRIPT[raw_pos:raw_pos + 2800]
        self.assertIn('$response.RawContentStream', raw_block)
        self.assertIn('[System.Text.Encoding]::UTF8.GetString', raw_block)

    def test_memory_degradation_requires_retryable_503_metadata(self):
        import_pos = SMOKE_SCRIPT.index('# Test 3: Import config (using exported bundle)')
        import_block = SMOKE_SCRIPT[import_pos:import_pos + 2400]
        self.assertIn('$importChunk.code -eq 503', import_block)
        self.assertIn('$importChunk.PSObject.Properties.Name -contains "retryAfter"', import_block)
        self.assertIn('Passed = ($importOk -or $importDegraded)', import_block)


if __name__ == "__main__":
    unittest.main()
