import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEPLOY_SCRIPT = ROOT / "scripts" / "deploy-config-bundle.ps1"
FLEET_DEPLOY_SCRIPT = ROOT / "scripts" / "deploy-multi-device.ps1"


class DeployConfigBundleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = DEPLOY_SCRIPT.read_text(encoding="utf-8")

    def test_comma_delimited_feature_argument_is_normalized(self):
        self.assertIn("foreach ($raw in @($Requested))", self.source)
        self.assertIn("-split ','", self.source)
        self.assertIn("$candidate.Trim()", self.source)

    def test_memory_profile_state_does_not_shadow_cli_parameter(self):
        self.assertIn('$script:appliedMemoryProfile = "none"', self.source)
        self.assertIn('$script:appliedMemoryProfile = Apply-MemoryProfile', self.source)
        self.assertNotIn('$script:memoryProfile =', self.source)

    def test_memory_profile_report_field_is_preserved(self):
        self.assertIn('memoryProfile = $script:appliedMemoryProfile', self.source)

    def test_classic_no_psram_disables_mqtt_before_staging(self):
        classic_block = self.source.index('if ($profile -eq "classic-no-psram") {', self.source.index('MQTTS staged as MQTT'))
        network_block = self.source.index('$network = $Docs["network.json"]', classic_block)
        mqtt_block = self.source[classic_block:network_block]
        self.assertIn('Set-PropertyValue $mqtt "enabled" $false', mqtt_block)
        self.assertIn('Set-PropertyValue $mqtt "disabled_by_memory" $true', mqtt_block)
        self.assertIn('Set-PropertyValue $mqtt "publishTopics" @()', mqtt_block)
        self.assertIn('Set-PropertyValue $mqtt "subscribeTopics" @()', mqtt_block)

    def test_mqtt_degradation_is_reported_separately_from_runtime_features(self):
        self.assertIn('mqttEnabled = $false', self.source)
        self.assertIn('mqttDisabledByMemory = $false', self.source)
        self.assertIn('$script:validation.mqttEnabled =', self.source)
        self.assertIn('$script:validation.mqttDisabledByMemory =', self.source)
        self.assertNotIn('$reasons["mqtt"] = "protocol.json enables MQTT"', self.source)

    def test_memory_profile_prunes_only_profile_disabled_runtime_entries(self):
        self.assertIn('$compactPeripherals = @($peripherals | Where-Object', self.source)
        self.assertIn('$compactRules = @($rules | Where-Object', self.source)
        self.assertIn('$script:memoryProfileDisabledRules -contains $rid', self.source)
        self.assertIn('sourcePeripherals = 0', self.source)
        self.assertIn('sourceRules = 0', self.source)

    def test_staging_excludes_repository_files_not_in_selected_bundle(self):
        clear_call = 'Clear-StagedConfiguration -ExpectedNames @($docs.Keys)'
        write_call = 'foreach ($name in $docs.Keys) { Write-Utf8Json'
        isolation_call = 'Test-StagedConfigurationIsolation -ExpectedNames @($docs.Keys)'
        self.assertIn('function Clear-StagedConfiguration', self.source)
        self.assertIn('Remove-Item -LiteralPath $file.FullName -Force -ErrorAction Stop', self.source)
        self.assertLess(self.source.index(clear_call), self.source.index(write_call))
        self.assertLess(self.source.index(write_call), self.source.index(isolation_call))
        self.assertIn('excludedRepositoryFiles = @($script:excludedRepositoryFiles | Sort-Object)', self.source)
        self.assertIn('stagedFiles = @($script:stagedHashes.Keys | Sort-Object)', self.source)

    def test_littlefs_only_mode_is_forwarded_and_audited(self):
        self.assertIn('[switch]$SkipFirmware', self.source)
        self.assertIn('if ($SkipFirmware) { $deployArgs += "-SkipFirmware" }', self.source)
        self.assertIn('skipFirmware = [bool]$SkipFirmware', self.source)

    def test_sta_credentials_are_injected_at_deployment_time(self):
        self.assertIn('[string]$StaSSID = $env:FASTBEE_STA_SSID', self.source)
        self.assertIn('[string]$StaPassword = $env:FASTBEE_STA_PASSWORD', self.source)
        self.assertIn('Apply-StaCredentials -Docs $docs', self.source)
        self.assertIn('staCredentialsInjected =', self.source)
        self.assertIn('staSsidSha256 =', self.source)

    def test_private_overlay_preserves_identity_without_replacing_hardware_topology(self):
        self.assertIn('[string]$PreserveConfigPath = ""', self.source)
        self.assertIn('function Merge-PreservedConfiguration', self.source)
        self.assertIn('("deviceId", "productNumber", "userId")', self.source)
        self.assertIn('("auth.json", "users.json")', self.source)
        self.assertIn('preservedFiles =', self.source)
        self.assertLess(
            self.source.index('Merge-PreservedConfiguration -Docs $docs'),
            self.source.index('Apply-StaCredentials -Docs $docs'),
        )

class MultiDeviceDeployTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = FLEET_DEPLOY_SCRIPT.read_text(encoding="utf-8-sig")

    def test_all_five_devices_have_unique_serial_targets(self):
        expected = {
            "ESP32-S3-F16R8": ("COM15", "esp32s3-F16R8"),
            "ESP32-C3-F4R0": ("COM4", "esp32c3-F4R0"),
            "ESP32-C6-F4R0": ("COM11", "esp32c6-F4R0"),
            "ESP32-F4R0": ("COM16", "esp32-F4R0"),
            "ESP32-S3-F8R0": ("COM7", "esp32s3-F8R0"),
        }
        for name, (port, env) in expected.items():
            self.assertIn(f'Name = "{name}"; Port = "{port}"; Env = "{env}"', self.source)

    def test_s3_f16_is_deployed_first_and_s3_f8_bundle_is_explicit(self):
        self.assertLess(self.source.index('Name = "ESP32-S3-F16R8"'),
                        self.source.index('Name = "ESP32-C3-F4R0"'))
        self.assertIn('"docs\\waveshare-esp32s3-relay6ch"', self.source)
        self.assertIn('[switch]$DeployS3F8', self.source)

    def test_fleet_refuses_identical_configuration_sources(self):
        self.assertIn('Configuration sources for $($device.Name)', self.source)
        self.assertIn('$hashOwners.ContainsKey($hash)', self.source)

    def test_skipping_serial_capture_keeps_sta_probe_enabled(self):
        deployment_checks = self.source.index('if (-not $DryRun -and -not $BuildOnly) {')
        serial_gate = self.source.index('if (-not $SkipSerialCheck) {', deployment_checks)
        sta_probe = self.source.index('$result.sta = Test-StaEndpoint', deployment_checks)
        self.assertLess(deployment_checks, serial_gate)
        self.assertLess(serial_gate, sta_probe)
        self.assertNotIn(
            'if (-not $DryRun -and -not $BuildOnly -and -not $SkipSerialCheck)',
            self.source,
        )

    def test_post_deployment_sta_probe_uses_serial_observation(self):
        self.assertIn('networkMode = $networkMode', self.source)
        self.assertIn('ip = $observedIp', self.source)
        self.assertIn('$device.StaIP = [string]$result.serial.ip', self.source)
        self.assertIn('$device.StaIP = ""', self.source)
        self.assertIn('No post-deployment STA address was observed', self.source)

    def test_report_and_banner_only_cover_selected_devices(self):
        self.assertIn('$selectedDevices = @($devices | Where-Object', self.source)
        self.assertIn('order = @($selectedDevices | ForEach-Object', self.source)
        self.assertIn('foreach ($device in $selectedDevices)', self.source)
        self.assertIn('$selectedDevices.Count)-device deployment', self.source)

    def test_failed_device_is_recorded_before_the_loop_can_stop(self):
        failure_start = self.source.index('$result.status = "failed"')
        catch_end = self.source.index('\n    }\n    $summary.results.Add', failure_start)
        failure_block = self.source[failure_start:catch_end]
        record_result = failure_block.index('$summary.results.Add([pscustomobject]$result)')
        stop_loop = failure_block.index('if (-not $SkipMissingPorts) { break }')
        continue_loop = failure_block.index('continue')
        self.assertLess(record_result, stop_loop)
        self.assertLess(stop_loop, continue_loop)

    def test_littlefs_only_mode_is_forwarded_and_audited(self):
        self.assertIn('[switch]$SkipFirmware', self.source)
        self.assertIn('if ($SkipFirmware) { $args += "-SkipFirmware" }', self.source)
        self.assertIn('skipFirmware = [bool]$SkipFirmware', self.source)

    def test_repository_examples_are_the_default_configuration_sources(self):
        self.assertNotIn('Resolve-ExternalConfig', self.source)
        self.assertNotIn('GetFolderPath("Desktop")', self.source)
        for source in (
            'docs\\device-test-configs\\com4-esp32c3-f4r0\\config',
            'docs\\c6-rgb-led-f4r0',
            'docs\\device-test-configs\\com16-esp32-f4r0\\config',
            'docs\\device-test-configs\\com15-esp32s3-f16r8\\config',
            'docs\\waveshare-esp32s3-relay6ch',
        ):
            self.assertIn(source, self.source)

    def test_fleet_can_use_explicit_ignored_private_overlay_root(self):
        self.assertIn('[string]$PreserveConfigRoot = ""', self.source)
        self.assertIn('Join-Path ([System.IO.Path]::GetFullPath($preserveRootCandidate)) $Device.Env', self.source)
        self.assertIn('$args += @("-PreserveConfigPath", $preservePath)', self.source)


if __name__ == "__main__":
    unittest.main()
