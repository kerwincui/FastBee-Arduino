"""Contracts for the persisted runtime feature snapshot and mDNS mirror."""

import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANAGER = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")
NETWORK = (ROOT / "src/network/NetworkManager.cpp").read_text(encoding="utf-8")
SYSTEM_ROUTE = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(
    encoding="utf-8"
)


def read_json(relative_path):
    return json.loads((ROOT / relative_path).read_text(encoding="utf-8"))


def registry_keys():
    return re.findall(
        r'registerFeature\(\s*(?:FeatureMeta\s*)?\{\s*"([^"]+)"',
        MANAGER,
    )


class FeatureConfigPersistenceContractTests(unittest.TestCase):
    def test_registry_factory_config_and_fixture_have_identical_keys(self):
        registered = registry_keys()
        source = read_json("data/config/features.json")
        fixture = read_json("test/fixtures/factory-config/features.json")

        self.assertEqual(len(registered), len(set(registered)), "duplicate registry key")
        self.assertEqual(set(registered), set(source["features"]))
        self.assertEqual(source, fixture)

    def test_factory_snapshot_has_only_management_services_enabled(self):
        config = read_json("data/config/features.json")
        network = read_json("data/config/network.json")
        self.assertEqual(1, config["version"])
        enabled = {
            key for key, value in config["features"].items() if value.get("enabled") is True
        }
        self.assertEqual({"webServer", "mdns"}, enabled)
        self.assertIs(network["enableMDNS"], config["features"]["mdns"]["enabled"])
        for value in config["features"].values():
            self.assertIs(type(value.get("enabled")), bool)

    def test_manager_writes_full_snapshot_and_normalizes_sparse_files(self):
        build_start = MANAGER.index("void RuntimeFeatureManager::buildConfigDoc")
        load_start = MANAGER.index("bool RuntimeFeatureManager::loadConfig", build_start)
        status_start = MANAGER.index("bool RuntimeFeatureManager::shouldEnable", load_start)
        build_body = MANAGER[build_start:load_start]
        load_body = MANAGER[load_start:status_start]

        self.assertIn("for (size_t i = 0; i < count_; ++i)", build_body)
        self.assertIn('entry["enabled"] = features_[i].userEnabled;', build_body)
        self.assertIn("recognizedEntries != count_", load_body)
        self.assertIn("saveCurrentConfig()", load_body)

    def test_network_mdns_compatibility_field_is_synchronized_to_features(self):
        update_start = NETWORK.index("bool FBNetworkManager::updateConfig")
        update_end = NETWORK.index("bool FBNetworkManager::updateConfigFromJSON", update_start)
        update_body = NETWORK[update_start:update_end]

        self.assertIn('setUserEnabled("mdns", effectiveConfig.enableMDNS', update_body)
        self.assertIn("runtimeFeatures.saveCurrentConfig()", update_body)
        self.assertIn("dnsManager->setMDNSEnabled(shouldRunMDNS())", update_body)
        self.assertRegex(
            NETWORK,
            r"bool FBNetworkManager::shouldRunMDNS\(\) const\s*\{[^}]*"
            r"wifiConfig\.enableMDNS[^}]*shouldEnable\(\"mdns\"\)",
        )
        self.assertNotRegex(
            NETWORK,
            r"if\s*\(\s*wifiConfig\.enableMDNS\s*(?:\)|&&|\|\|)",
        )

    def test_factory_reset_restores_feature_defaults(self):
        reset_start = SYSTEM_ROUTE.index('/api/system/factory-reset')
        reset_end = SYSTEM_ROUTE.index("request->send(response);", reset_start)
        reset_body = SYSTEM_ROUTE[reset_start:reset_end]
        self.assertIn("RuntimeFeatureManager::instance().resetToDefaults()", reset_body)
        self.assertIn("resetCount++", reset_body)


if __name__ == "__main__":
    unittest.main()
