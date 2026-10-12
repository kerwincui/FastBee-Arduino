"""Whole-file exact reverse-map for 64 MQTT nested fixed-field writes.

Source-equivalence evidence only: no new route behavioral coverage is claimed.
The three later literal-match bridge edits and 16 staging writes are reversed
exactly to their reviewed originals
before checking the historical whole-file hash; unreviewed drift still fails.
The shared helper's real ArduinoJson differential/OOM suite covers its semantics.
"""
import hashlib
import json
import os
from pathlib import Path
import unittest

from test_mqtt_test_fixed_json import fixed_assignments, SIGNATURES, ORACLE_SHA256
from test_periph_exec_fixed_json import definition

ROOT = Path(os.environ.get("FASTBEE_TEST_ROOT", next(
    path for path in Path(__file__).resolve().parents if (path / "platformio.ini").is_file())))
HERE = Path(__file__).resolve().parent
CPP = HERE if (HERE / "mqtt_nested_write_manifest.json").is_file() else ROOT / "test/unit/cpp"
MANIFEST_SHA256 = "8f9bc0eee322554f93cdcf127f74d3778148e95776c346f0b7053374c7db2ef3"
BEFORE_SHA256 = "f01a68541ba43d42cdd60fbec162f91e07d6e8f106202edad5280b0b7dddc19a"


class MqttNestedFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest_bytes = (CPP / "mqtt_nested_write_manifest.json").read_bytes()
        cls.manifest = json.loads(cls.manifest_bytes.decode("utf-8"))
        cls.current = (ROOT / cls.manifest["sourcePath"]).read_text(encoding="utf-8")

    def test_manifest_is_frozen(self):
        self.assertEqual(MANIFEST_SHA256, hashlib.sha256(self.manifest_bytes).hexdigest())
        self.assertEqual(BEFORE_SHA256, self.manifest["normalizedBeforeSha256"])

    def historical_input(self):
        oracle = (CPP / "mqtt_test_fixed_json_before.inc").read_text(encoding="utf-8")
        self.assertEqual(ORACLE_SHA256, hashlib.sha256(oracle.encode("utf-8")).hexdigest())
        restored = self.current
        # The capacity bridge is independently covered by its differential suite.
        # Reverse only its three exact edits before the frozen historical offsets.
        bridge_edits = (
            ('#include "network/LiteralStringMatch.h"\n', ''),
            ('FastBee::stringEqualsIgnoreCaseLiteral(authTypeStr, "ENCRYPTED")',
             'authTypeStr.equalsIgnoreCase("ENCRYPTED")'),
            ('FastBee::stringEqualsIgnoreCaseLiteral(authTypeStr, "SIMPLE")',
             'authTypeStr.equalsIgnoreCase("SIMPLE")'),
        )
        for replacement, original in bridge_edits:
            self.assertEqual(1, restored.count(replacement),
                             "Later literal-match bridge edit is not unique")
            restored = restored.replace(replacement, original, 1)
        for signature, expected_count in zip(SIGNATURES, (3, 13)):
            original = definition(oracle, signature)
            replacement, count = fixed_assignments(original)
            self.assertEqual(expected_count, count)
            current_method = definition(restored, signature)
            self.assertEqual(replacement, current_method,
                             "Later staging method drifted beyond its reviewed writes")
            self.assertEqual(1, restored.count(current_method))
            restored = restored.replace(current_method, original, 1)
        return restored

    def test_whole_file_reverse_preserves_every_other_character(self):
        changes = self.manifest["changes"]
        self.assertEqual(64, len(changes))
        current = self.historical_input()
        shift = 0
        spans = []
        counts = {}
        for change in changes:
            self.assertEqual("JsonDocument", change["rootType"])
            self.assertEqual(2, len(change["keys"]))
            self.assertIn(change["root"], ("doc", "busy", "resp"))
            start = change["offset"] + shift
            replacement = change["replacement"]
            self.assertEqual(replacement, current[start:start + len(replacement)])
            spans.append((start, len(replacement), change["original"]))
            shift += len(replacement) - len(change["original"])
            counts[change["method"]] = counts.get(change["method"], 0) + 1
        self.assertEqual({"handleTestMqttConnection": 25, "handleMqttReconnect": 33,
                          "handleMqttDisconnect": 5, "handleMqttNtpSync": 1}, counts)
        restored = current
        for start, length, original in reversed(spans):
            restored = restored[:start] + original + restored[start + length:]
        self.assertEqual(BEFORE_SHA256, hashlib.sha256(restored.encode("utf-8")).hexdigest(),
                         "Full file differs outside the 64 exact replacements")
        # Forward replay confirms identical call order and unchanged RHS,
        # including the multiline ternary and both string-internal semicolons.
        replay = restored
        for change in reversed(changes):
            start = change["offset"]
            self.assertEqual(change["original"], replay[start:start + len(change["original"])])
            replay = replay[:start] + change["replacement"] + replay[start + len(change["original"]):]
        self.assertEqual(current, replay)

    def test_complex_rhs_remains_complete(self):
        values = [entry["rhs"] for entry in self.manifest["changes"]]
        self.assertEqual(1, sum("\n" in rhs for rhs in values))
        self.assertEqual(2, sum(";" in rhs for rhs in values))
        self.assertIn('doc["data"]["errorMessage"]', values)


if __name__ == "__main__":
    unittest.main()
