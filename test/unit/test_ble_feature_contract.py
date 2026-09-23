"""Contracts for the optional BLE provisioning runtime gate."""

import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
FRAMEWORK = (ROOT / "src/core/FastBeeFramework.cpp").read_text(encoding="utf-8")
PROVISION = (ROOT / "src/network/handlers/ProvisionRouteHandler.cpp").read_text(encoding="utf-8")
FEATURES = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")


def braced_block(source: str, marker: str) -> str:
    marker_at = source.index(marker)
    block_start = source.index("{", marker_at + len(marker))
    depth = 0
    for index in range(block_start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[block_start + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


class BleFeatureContractTests(unittest.TestCase):
    def test_ble_is_optional_and_disabled_by_default(self):
        start = FEATURES.index('registerFeature({"ble",')
        entry = FEATURES[start : start + 320]
        self.assertIn("FASTBEE_ENABLE_BLE != 0, false", entry)

    def test_startup_does_not_allocate_ble_when_runtime_feature_is_disabled(self):
        block = braced_block(
            FRAMEWORK,
            'if (FastBee::RuntimeFeatureManager::instance().shouldEnable("ble"))',
        )
        self.assertIn("bleProvisioner.reset(new BLEProvisioner(network.get()))", block)
        self.assertIn("webConfig->setBLEProvisioner(bleProvisioner.get())", block)
        self.assertIn("bleProvisioner->begin(false)", block)

        gate = FRAMEWORK.index(
            'if (FastBee::RuntimeFeatureManager::instance().shouldEnable("ble"))'
        )
        allocation = FRAMEWORK.index("bleProvisioner.reset(new BLEProvisioner", gate)
        self.assertLess(gate, allocation)

    def test_api_start_is_gated_but_stop_remains_available_for_cleanup(self):
        start = braced_block(
            PROVISION,
            "void ProvisionRouteHandler::handleBleProvisionStart(",
        )
        stop = braced_block(
            PROVISION,
            "void ProvisionRouteHandler::handleBleProvisionStop(",
        )
        self.assertIn('shouldEnable("ble")', start)
        self.assertLess(start.index('shouldEnable("ble")'), start.index("bleProvisioner->begin(true)"))
        self.assertIn("bleProvisioner->stop()", stop)
        self.assertNotIn('shouldEnable("ble")', stop)

    def test_status_distinguishes_compiled_enabled_and_supported(self):
        status = braced_block(
            PROVISION,
            "void ProvisionRouteHandler::handleBleProvisionStatus(",
        )
        for field in ('data["compiled"]', 'data["enabled"]', 'data["supported"]'):
            self.assertIn(field, status)


if __name__ == "__main__":
    unittest.main()
