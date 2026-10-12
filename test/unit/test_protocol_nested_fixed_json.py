"""Source-preservation check for the 44 nested fixed-field call sites.

The real AJ proxy/OOM behavior is covered by the shared-helper suite. This
test freezes the complete two production methods and proves that only the
reviewed writes changed. It is not a full HTTP/FS/lifecycle behavior fixture.
"""
import hashlib
import os
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(os.environ.get("FASTBEE_TEST_ROOT", next(
    path for path in Path(__file__).resolve().parents if (path / "platformio.ini").is_file())))
HERE = Path(__file__).resolve().parent
CPP = HERE if (HERE / "protocol_nested_write_before.inc").is_file() else ROOT / "test/unit/cpp"
sys.path.insert(0, str(ROOT / "test/unit"))
from test_sensor_publication import definition

ORACLE_SHA256 = "6ca7a9591826d9dc08f2fdcf8aaad90cb639da65946e7b53351d6be5221156b7"
ASSIGNMENT = re.compile(
    r'\b(doc|resp)\["([^"\n]+)"\]\["([^"\n]+)"\](\s*)=(?!=)(\s*)([^;\n]+);')
CALL = re.compile(
    r'\bFastBee::setFixedJsonField\((doc|resp), "([^"\n]+)", "([^"\n]+)", ([^;\n]+)\);')


class ProtocolNestedFixedJsonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = (ROOT / "src/network/handlers/ProtocolRouteHandler.cpp").read_text(encoding="utf-8")
        cls.oracle_path = CPP / "protocol_nested_write_before.inc"
        cls.oracle_bytes = cls.oracle_path.read_bytes()
        cls.oracle = cls.oracle_bytes.decode("utf-8").replace("\r\n", "\n")

    def test_frozen_complete_methods_are_unchanged(self):
        self.assertEqual(ORACLE_SHA256, hashlib.sha256(self.oracle_bytes).hexdigest())

    def check_method(self, signature, expected_count):
        before = definition(self.oracle, signature)
        current = definition(self.source, signature)
        assignments = list(ASSIGNMENT.finditer(before))
        calls = list(CALL.finditer(current))
        self.assertEqual(expected_count, len(assignments))
        self.assertEqual(expected_count, len(calls))
        expected = ASSIGNMENT.sub(
            lambda m: f'FastBee::setFixedJsonField({m[1]}, "{m[2]}", "{m[3]}", {m[6]});',
            before)
        self.assertEqual(expected, current, "Non-call bytes, RHS or ordering changed")
        # Restore each exact original assignment, including alignment whitespace.
        # The destination/key/RHS comparison prevents a reordered or renamed
        # field from being hidden by reconstruction from the oracle.
        restored, position = [], 0
        for old, call in zip(assignments, calls):
            self.assertEqual((old[1], old[2], old[3], old[6]),
                             (call[1], call[2], call[3], call[4]))
            restored.extend((current[position:call.start()], old.group()))
            position = call.end()
        restored.append(current[position:])
        self.assertEqual(before, "".join(restored),
                         "Full method reverse transformation is not exact")

    def test_complete_normalizer_preserves_all_non_call_bytes(self):
        self.check_method("void normalizeProtocolConfig(", 4)

    def test_complete_save_preserves_all_non_call_bytes(self):
        self.check_method("void ProtocolRouteHandler::handleSaveProtocolConfig(", 40)


if __name__ == "__main__":
    unittest.main()
