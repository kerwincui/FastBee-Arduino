"""Host contracts for the NetworkManager/runtime-feature mDNS boundary.

The native test environment does not compile FBNetworkManager. These tests keep
the migration and two-file persistence rules tied to the production function
bodies without requiring an ESP32 SDK or a live filesystem.
"""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
NETWORK = (ROOT / "src/network/NetworkManager.cpp").read_text(encoding="utf-8")
FEATURES = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")


def function_body(source, signature, next_signature):
    start = source.index(signature)
    end = source.index(next_signature, start + len(signature))
    return source[start:end]


def braced_block(source, marker):
    marker_at = source.index(marker)
    block_start = source.index("{", marker_at + len(marker))
    depth = 0
    for index in range(block_start, len(source)):
        char = source[index]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[block_start + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


def assert_in_order(test_case, source, *fragments):
    position = -1
    for fragment in fragments:
        next_position = source.find(fragment, position + 1)
        test_case.assertGreater(
            next_position,
            position,
            f"missing or out-of-order fragment: {fragment}",
        )
        position = next_position


class NetworkMdnsFeatureContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.initialize = function_body(
            NETWORK,
            "bool FBNetworkManager::initialize()",
            "void FBNetworkManager::disconnect()",
        )
        cls.update_config = function_body(
            NETWORK,
            "bool FBNetworkManager::updateConfig(",
            "bool FBNetworkManager::updateConfigFromJSON(",
        )
        cls.load_features = function_body(
            FEATURES,
            "bool RuntimeFeatureManager::loadConfig()",
            "bool RuntimeFeatureManager::shouldEnable(",
        )
        cls.set_user_enabled = function_body(
            FEATURES,
            "bool RuntimeFeatureManager::setUserEnabled(",
            "bool RuntimeFeatureManager::resetToDefaults(",
        )

    def test_sparse_snapshot_keeps_missing_mdns_eligible_for_legacy_migration(self):
        register_feature = function_body(
            FEATURES,
            "void RuntimeFeatureManager::registerFeature(",
            "RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(",
        )
        parsed_entry = braced_block(self.load_features, "for (JsonPairConst kv : features)")
        canonical_rewrite = braced_block(
            self.load_features,
            "if (needsCanonicalRewrite && !needsLegacyMDNSMigration)",
        )
        migration = braced_block(
            self.initialize,
            'if (!runtimeFeatures.hasExplicitSetting("mdns"))',
        )

        self.assertIn("st.explicitlyConfigured = false;", register_feature)
        self.assertIn("f->explicitlyConfigured = true;", parsed_entry)
        self.assertEqual(1, self.load_features.count("f->explicitlyConfigured = true;"))
        self.assertIn("saveCurrentConfig()", canonical_rewrite)
        self.assertNotIn("explicitlyConfigured", canonical_rewrite)
        self.assertIn('setUserEnabled("mdns", wifiConfig.enableMDNS)', migration)
        # Saving is still required when the legacy value equals the default:
        # it turns the old sparse file into a complete authoritative snapshot.
        self.assertNotIn("migrated", migration)
        self.assertIn("runtimeFeatures.saveCurrentConfig()", migration)

    def test_explicit_features_mdns_overrides_stale_network_mirror(self):
        migration_guard = 'if (!runtimeFeatures.hasExplicitSetting("mdns"))'
        migration_end = self.initialize.index(
            "const bool configuredMDNS", self.initialize.index(migration_guard)
        )
        migration_region = self.initialize[
            self.initialize.index(migration_guard) : migration_end
        ]

        self.assertNotIn("wifiConfig.enableMDNS =", migration_region)
        assert_in_order(
            self,
            self.initialize,
            migration_guard,
            'const bool configuredMDNS = runtimeFeatures.isUserEnabled("mdns");',
            "if (wifiConfig.enableMDNS != configuredMDNS)",
            "wifiConfig.enableMDNS = configuredMDNS;",
            "networkConfigNeedsSave = true;",
            "if (networkConfigNeedsSave && !saveNetworkConfig())",
        )

    def test_update_rolls_back_when_either_config_file_save_fails(self):
        feature_save_failure = braced_block(
            self.update_config,
            "if (mdnsFeatureChanged && !runtimeFeatures.saveCurrentConfig())",
        )
        network_save_failure = braced_block(
            self.update_config, "if (!saveSuccess)"
        )

        assert_in_order(
            self,
            self.update_config,
            'const bool previousFeatureMDNS = runtimeFeatures.isUserEnabled("mdns");',
            "const WiFiConfig previousConfig = wifiConfig;",
            "wifiConfig = effectiveConfig;",
            "if (mdnsFeatureChanged && !runtimeFeatures.saveCurrentConfig())",
            "saveSuccess = saveNetworkConfig();",
            "if (!saveSuccess)",
            "dnsManager->setMDNSEnabled(shouldRunMDNS());",
        )
        assert_in_order(
            self,
            feature_save_failure,
            "runtimeFeatures.restoreUserConfig(previousFeatureState);",
            "wifiConfig = previousConfig;",
            "return false;",
        )
        assert_in_order(
            self,
            network_save_failure,
            "wifiConfig = previousConfig;",
            "if (mdnsFeatureChanged)",
            "runtimeFeatures.restoreUserConfig(previousFeatureState);",
            "runtimeFeatures.saveCurrentConfig()",
            "return false;",
        )

    def test_uncompiled_mdns_enable_is_rejected_and_runtime_gate_stays_closed(self):
        # The label is localized in production; anchor the registration by key.
        mdns_at = FEATURES.index('registerFeature({"mdns",')
        mdns_entry = FEATURES[mdns_at : mdns_at + 300]
        request_change = braced_block(
            self.update_config,
            "if (effectiveConfig.enableMDNS != previousFeatureMDNS)",
        )
        should_run = function_body(
            NETWORK,
            "bool FBNetworkManager::shouldRunMDNS() const",
            "NetworkStatusInfo FBNetworkManager::getStatusInfo() const",
        )

        self.assertIn("FASTBEE_ENABLE_MDNS != 0", mdns_entry)
        self.assertIn(
            "if (enabled && !feature->meta.compiled) return false;",
            self.set_user_enabled,
        )
        self.assertIn(
            'if (!runtimeFeatures.setUserEnabled("mdns", effectiveConfig.enableMDNS,',
            request_change,
        )
        self.assertIn("effectiveConfig.enableMDNS = previousFeatureMDNS;", request_change)
        self.assertIn("wifiConfig.enableMDNS", should_run)
        self.assertIn('shouldEnable("mdns")', should_run)


if __name__ == "__main__":
    unittest.main()
