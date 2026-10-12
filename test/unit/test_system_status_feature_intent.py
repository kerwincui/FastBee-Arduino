"""Run the production compact-status feature block with real ArduinoJson.

Admission methods, fixed-field writes and the overflow rejection are extracted
from production, not reproduced in a test serializer. No device is contacted.
"""
from pathlib import Path
import re
import sys
import unittest

ROOT = next(path for path in Path(__file__).resolve().parents
            if (path / "platformio.ini").is_file())
sys.path.insert(0, str(ROOT / "test/unit"))
from test_feature_network_fixed_json import SerializerFixtureBase
from test_sensor_publication import definition


class PublicStatusActivityContractTests(unittest.TestCase):
    @staticmethod
    def verify_activity_dominates_responses(handler):
        # This contract reads the real handler, including all cache, overflow
        # and serialization rejection branches. Every branch must be reached
        # only after the shared activity hook, without introducing auth.
        body = handler[handler.index("{") + 1:]
        body = re.sub(r"/\*.*?\*/|//[^\n]*", "", body, flags=re.S).lstrip()
        if not body.startswith("ctx->noteWebRequestActivity();"):
            raise AssertionError("Public status must record activity at entry")
        if body.count("ctx->noteWebRequestActivity();") != 1:
            raise AssertionError("Public status must record each request once")
        if "requireAuth(" in body:
            raise AssertionError("Public status must preserve unauthenticated identity access")
        for marker in ("if (_statusCache.valid)", "if (wrapper.overflowed())",
                       "if (written != expectedSize", "request->send(response);"):
            if marker not in body:
                raise AssertionError("Real response/rejection path missing: " + marker)

    def production_handler(self):
        route = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(encoding="utf-8")
        return definition(route, "void SystemRouteHandler::handleSystemStatus(")

    def test_entry_activity_covers_cache_fresh_and_rejected_responses(self):
        self.verify_activity_dominates_responses(self.production_handler())

    def test_original_missing_activity_is_rejected(self):
        handler = self.production_handler().replace("ctx->noteWebRequestActivity();", "", 1)
        with self.assertRaisesRegex(AssertionError, "at entry"):
            self.verify_activity_dominates_responses(handler)

    def test_cache_only_activity_is_rejected(self):
        handler = self.production_handler().replace("ctx->noteWebRequestActivity();", "", 1)
        handler = handler.replace("if (_statusCache.valid) {",
                                  "if (_statusCache.valid) { ctx->noteWebRequestActivity();", 1)
        with self.assertRaisesRegex(AssertionError, "at entry"):
            self.verify_activity_dominates_responses(handler)


class SystemStatusFeatureIntentTests(SerializerFixtureBase):
    @classmethod
    def setUpClass(cls):
        cls.prepare(("system_status_feature_intent_harness.cpp",), (
            "src/network/handlers/SystemRouteHandler.cpp",
            "src/core/RuntimeFeatureManager.cpp",
            "include/core/RuntimeFeatureManager.h",
            "include/core/FeatureBudget.h",
            "include/core/FeatureInstanceRegistry.h",
            "test/unit/test_system_status_feature_intent.py",
        ))
        route = (ROOT / "src/network/handlers/SystemRouteHandler.cpp").read_text(encoding="utf-8")
        handler = definition(route, "void SystemRouteHandler::handleSystemStatus(")
        marker = "auto& rfm = FastBee::RuntimeFeatureManager::instance();"
        if handler.count(marker) != 1:
            raise AssertionError("Expected one production feature traversal")
        start = handler.rfind("{", 0, handler.index(marker))
        block = definition(handler[start:], "{")
        overflow = definition(handler, "if (wrapper.overflowed())")
        if handler.index("if (wrapper.overflowed())") < handler.index(marker):
            raise AssertionError("Overflow must reject after the feature writes")
        (cls.temp / "system_status_feature_block.inc").write_text(
            block + "\n" + overflow, encoding="utf-8")
        manager = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")
        signatures = (
            "const RuntimeFeatureManager::FeatureState* RuntimeFeatureManager::findFeature(",
            "bool RuntimeFeatureManager::isUserEnabled(",
            "bool RuntimeFeatureManager::isRuntimeDisabled(",
            "bool RuntimeFeatureManager::shouldEnable(",
            "const char* RuntimeFeatureManager::admissionState(",
        )
        (cls.temp / "system_status_feature_methods.inc").write_text(
            "\n\n".join(definition(manager, signature) for signature in signatures), encoding="utf-8")
        cls.binary = cls.compile("system_status_feature_intent_harness.cpp", "system-feature-intent",
                                 ["-DARDUINOJSON_ENABLE_ARDUINO_STRING=0"])

    def test_compiled_and_user_intent_survive_admission_changes(self):
        self.check(self.binary, "states")

    def test_absent_panel_does_not_fabricate_intent_or_change_other_features(self):
        self.check(self.binary, "absent")

    def test_allocation_limits_reject_overflow_instead_of_valid_status(self):
        self.check(self.binary, "allocation")


if __name__ == "__main__":
    unittest.main()
