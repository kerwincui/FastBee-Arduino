"""Regression checks for the live-device Web asset smoke test."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = (ROOT / "scripts/device-web-smoke.ps1").read_text(encoding="utf-8")


class DeviceWebSmokeContractTests(unittest.TestCase):
    def test_fullscreen_page_and_runtime_assets_are_probed(self):
        required_paths = (
            "/pages/fullscreen.html",
            "/js/chunk-1-core.js",
            "/js/chunk-2-app.js",
            "/js/chunk-3-state.js",
            "/js/modules/dashboard-fullscreen.js",
        )
        for path in required_paths:
            with self.subTest(path=path):
                self.assertIn(f'"{path}"', SCRIPT)

    def test_probe_requires_http_200_and_successful_transfer(self):
        self.assertIn("$curlExit -eq 0", SCRIPT)
        self.assertIn("$status -eq 200", SCRIPT)
        self.assertIn("$contentComplete", SCRIPT)
        self.assertIn("if ($failed.Count -gt 0) { exit 1 }", SCRIPT)

    def test_transport_failure_is_preserved_when_latency_also_fails(self):
        transport = "$probeErrors.Add((Get-CurlTransportError -ExitCode $curlExit))"
        latency = '$probeErrors.Add("latency ${elapsedMs}ms exceeds limit ${MaxElapsedMs}ms")'
        joined = '$errorText = $probeErrors -join "; "'
        self.assertIn(transport, SCRIPT)
        self.assertIn(latency, SCRIPT)
        self.assertIn(joined, SCRIPT)
        self.assertLess(SCRIPT.index(transport), SCRIPT.index(latency))
        self.assertLess(SCRIPT.index(latency), SCRIPT.index(joined))
        self.assertIn('(curl exit=$ExitCode)', SCRIPT)
        self.assertNotIn('$errorText = "latency ${elapsedMs}ms', SCRIPT)

    def test_known_content_length_is_checked_against_downloaded_bytes(self):
        self.assertIn("--dump-header $headerPath", SCRIPT)
        self.assertIn("Get-ResponseContentLength -HeaderPath $headerPath", SCRIPT)
        self.assertIn("$bytes -ne [int64]$contentLength", SCRIPT)
        self.assertIn("incomplete body: received", SCRIPT)
        self.assertIn("ContentLength = $contentLength", SCRIPT)
        self.assertIn("CurlExit = $curlExit", SCRIPT)

    def test_console_endpoint_label_does_not_include_url_credentials(self):
        self.assertIn("function Get-SafeEndpointLabel", SCRIPT)
        self.assertIn("$uri.Host", SCRIPT)
        self.assertIn("$displayBaseUrl = Get-SafeEndpointLabel -Url $BaseUrl", SCRIPT)
        self.assertIn('Write-Host "Web page smoke: $displayBaseUrl', SCRIPT)
        self.assertNotIn('Write-Host "Web page smoke: $BaseUrl', SCRIPT)

    def test_probe_uses_the_complete_compile_time_manifest(self):
        self.assertIn("/web-resource-manifest.json", SCRIPT)
        self.assertIn("$manifestPages.Add($normalized)", SCRIPT)
        self.assertIn("$manifestPages | Sort-Object -Unique", SCRIPT)
        self.assertIn('"/"', SCRIPT)
        self.assertNotIn("$pages | Where-Object", SCRIPT)
        self.assertIn("falling back to the legacy probe list", SCRIPT)

    def test_release_validation_can_require_the_manifest(self):
        self.assertIn("[switch]$RequireManifest", SCRIPT)
        self.assertIn("if ($RequireManifest)", SCRIPT)
        self.assertIn("Required Web resource manifest is unavailable", SCRIPT)
        self.assertIn("manifest request failed", SCRIPT)
        self.assertIn("manifest declared no resources", SCRIPT)
        self.assertIn("manifest contained no usable resource paths", SCRIPT)


if __name__ == "__main__":
    unittest.main()
