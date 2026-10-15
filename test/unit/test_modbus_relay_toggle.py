"""Modbus relay ctrl commands must support "toggle".

Regression: the periph-exec ctrl path ({"ctrl":[{"d":0,"c":0,"a":"on"}]})
rejected any action that was not boolean-like, so a panel/linkage rule could
not toggle a Modbus relay (only force on/off). The write layer
(ModbusHandler::writeOrdinaryRelayOnce) already implements read-invert-write
toggle under the same ordinary-control admission; the ctrl branch must accept
"a":"toggle" for mapped relay targets and pass it through.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ModbusRelayToggleTests(unittest.TestCase):
    def test_ctrl_branch_accepts_toggle_for_mapped_relays(self):
        from test.unit.test_screen_snapshot_cache import definition
        source = (ROOT / "src/core/PeriphExecExecutor.cpp").read_text(encoding="utf-8")
        marker = 'if (devType == "relay") {'
        start = source.index(marker)
        end = source.index('if (devType == "pwm"', start)
        branch = source[start:end]
        self.assertIn('"toggle"', branch)
        # Toggle must reuse the mapped-relay submission so the read-invert-write
        # happens inside the same ordinary-control context (physical coil
        # toggle equals logical toggle regardless of ncMode).
        self.assertIn("writeRelayWithinBudget", branch)
        submit = definition(source, "auto writeRelayWithinBudget")
        self.assertIn("toggle", submit)
        # Boolean parsing stays for explicit on/off values.
        self.assertIn("tryParseBoolLike", branch)
        # Unknown actions are still rejected.
        self.assertIn("CONTROL_PARAMETER_INVALID", branch)

    def test_toggle_is_not_parsed_as_boolean(self):
        # Pin the separation: "toggle" is a distinct command, not a truthy word.
        from test.unit.test_screen_snapshot_cache import definition
        utils = (ROOT / "src/utils/StringUtils.cpp").read_text(encoding="utf-8")
        body = definition(utils, "bool StringUtils::tryParseBoolLike")
        self.assertNotIn('"toggle"', body)


if __name__ == "__main__":
    unittest.main()
