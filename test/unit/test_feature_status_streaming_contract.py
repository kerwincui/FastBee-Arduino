"""Regression checks for low-memory feature status responses."""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
ROUTE = (ROOT / "src/network/handlers/FeatureRouteHandler.cpp").read_text(encoding="utf-8")
MANAGER = (ROOT / "src/core/RuntimeFeatureManager.cpp").read_text(encoding="utf-8")
STATE = (ROOT / "web-src/js/state.js").read_text(encoding="utf-8")


class FeatureStatusStreamingContractTests(unittest.TestCase):
    def _status_body(self):
        start = ROUTE.index("void FeatureRouteHandler::handleStatus")
        end = ROUTE.index("void FeatureRouteHandler::handleConfig", start)
        return ROUTE[start:end]

    def test_status_uses_bounded_per_item_chunked_response(self):
        body = self._status_body()
        self.assertIn("getFeatureStatusJson(i, itemDoc.to<JsonObject>())", body)
        self.assertIn("HandlerUtils::sendJsonListChunked", body)
        self.assertNotIn("getStatusJson(", body)
        self.assertNotIn("HandlerUtils::sendJsonStream", body)

    def test_serialization_failure_is_not_returned_as_http_200(self):
        body = self._status_body()
        self.assertIn("itemDoc.overflowed()", body)
        self.assertRegex(body, r"written\s*!=\s*expectedSize")
        self.assertIn("sendJsonError(request, 503", body)

    def test_streamed_and_full_status_share_active_logic(self):
        feature_start = MANAGER.index("bool RuntimeFeatureManager::getFeatureStatusJson")
        summary_start = MANAGER.index("void RuntimeFeatureManager::getStatusSummaryJson", feature_start)
        full_start = MANAGER.index("void RuntimeFeatureManager::getStatusJson", summary_start)
        feature_body = MANAGER[feature_start:summary_start]
        full_body = MANAGER[full_start:]
        self.assertIn("const bool active = shouldEnable(st.meta.key);", feature_body)
        self.assertIn("getFeatureStatusJson(i, arr.add<JsonObject>())", full_body)
        self.assertIn("getStatusSummaryJson", full_body)

    def test_menu_contract_remains_compiled_and_active(self):
        self.assertIn("f.compiled !== false && f.active === true", STATE)
        self.assertIn("setMenuVisible('device', true);", STATE)


if __name__ == "__main__":
    unittest.main()
