"""Contracts for the optional device-control aggregate backend.

The native harness does not provide ESPAsyncWebServer. These source-level
checks keep the route boundary and the allocation strategy explicit while the
firmware build supplies the C++ compile check.
"""

import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
ROUTE = (
    ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
).read_text(encoding="utf-8")


def function_body(source: str, signature: str) -> str:
    match = re.search(signature + r"[^;{]*\{", source, re.S)
    if match is None:
        raise AssertionError(f"missing C++ function matching {signature!r}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated C++ function matching {signature!r}")


class DeviceControlBackendContractTests(unittest.TestCase):
    def test_only_the_dashboard_aggregate_is_feature_gated(self):
        controls = function_body(
            ROUTE, r"\bPeriphExecRouteHandler::handleGetControls\s*\("
        )
        auth = controls.index("requireAuth(request)")
        gate = controls.index('shouldEnable("deviceControl")')
        manager = controls.index("PeriphExecManager::getInstance()")
        self.assertLess(auth, gate)
        self.assertLess(gate, manager)
        self.assertIn("Device control disabled by runtime feature config", controls)
        # A single occurrence in the entire route implementation proves the
        # feature gate was not copied into CRUD, rule execution, or event APIs.
        self.assertEqual(1, ROUTE.count('shouldEnable("deviceControl")'))

    def test_controls_use_per_item_grouped_chunking(self):
        controls = function_body(
            ROUTE, r"\bPeriphExecRouteHandler::handleGetControls\s*\("
        )
        stream = function_body(ROUTE, r"\bsendControlsChunked\s*\(")
        self.assertIn("std::vector<GroupedControlItem> items", controls)
        self.assertIn("for (uint8_t group = 0", controls)
        self.assertIn("sendControlsChunked(request, std::move(items))", controls)
        self.assertIn("beginChunkedResponse", stream)
        self.assertIn("item = String()", stream)
        self.assertNotRegex(
            controls,
            r"String\s+(?:gpioItems|modbusItems|systemItems|scriptItems|sensorItems|otherItems)",
        )
        self.assertNotIn("request->send(200, \"application/json\", json)", controls)

    def test_sensor_cache_is_snapshotted_at_most_once_per_request(self):
        controls = function_body(
            ROUTE, r"\bPeriphExecRouteHandler::handleGetControls\s*\("
        )
        snapshot = controls.index("getSensorReadCacheCopy()")
        grouped_build = controls.index("for (uint8_t group = 0")
        self.assertEqual(1, controls.count("getSensorReadCacheCopy()"))
        self.assertLess(snapshot, grouped_build)
        self.assertIn("if (needsSensorCache)", controls)

    def test_every_dynamic_control_field_uses_checked_json_escaping(self):
        builder = function_body(ROUTE, r"\bbuildControlItemJson\s*\(")
        for value in (
            "rule.id",
            "rule.name",
            "firstAction.targetPeriphId",
            "peripheralName",
            "cacheKey",
            "label",
            "unit",
            "cached->second.value",
        ):
            self.assertIn(f"writer.appendQuoted({value})", builder)

        writer = ROUTE[
            ROUTE.index("class ControlJsonWriter") : ROUTE.index(
                "struct GroupedControlItem"
            )
        ]
        for escape in ('case \'"\'', "case '\\\\'", "case '\\b'", "case '\\f'", "case '\\n'", "case '\\r'", "case '\\t'"):
            self.assertIn(escape, writer)
        self.assertIn('"\\\\u%04x"', writer)
        self.assertIn("CONTROL_ITEM_MAX_BYTES", writer)
        self.assertIn("!value_.concat", writer)

    def test_low_memory_and_serialization_failures_return_valid_fallbacks(self):
        pressure = function_body(ROUTE, r"\bshouldDegradeControlsResponse\s*\(")
        controls = function_body(
            ROUTE, r"\bPeriphExecRouteHandler::handleGetControls\s*\("
        )
        degraded = function_body(ROUTE, r"\bsendDegradedControlsResponse\s*\(")
        self.assertIn("MemoryBudget::dram8Free()", pressure)
        self.assertIn("MemoryBudget::dram8Largest()", pressure)
        self.assertIn("buildControlItemJson", controls)
        self.assertIn("if (buildFailed)", controls)
        self.assertIn("stream allocation failed", controls)
        for group in ("gpio", "modbus", "system", "script", "sensor", "other"):
            self.assertIn(f'\\"{group}\\":[]', degraded)


if __name__ == "__main__":
    unittest.main()
