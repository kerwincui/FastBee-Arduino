"""Regression checks for non-intrusive serial log capture."""

from __future__ import annotations

import pathlib
import importlib.util
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = pathlib.Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "scripts" / "serial-diagnostics.py").read_text(encoding="utf-8")


class SerialDiagnosticsContractTests(unittest.TestCase):
    def test_modem_control_is_deasserted_before_open(self):
        start = SOURCE.index("def open_serial")
        end = SOURCE.index("def capture_logs", start)
        block = SOURCE[start:end]
        create = block.index("stream = serial.Serial()")
        dtr = block.index('stream.dtr = getattr(args, "assert_dtr", False)')
        rts = block.index("stream.rts = False")
        opened = block.index("stream.open()")
        self.assertLess(create, dtr)
        self.assertLess(dtr, opened)
        self.assertLess(rts, opened)
        self.assertNotIn("serial.Serial(\n                args.port", block)

    def test_native_cdc_dtr_is_opt_in_and_rts_stays_inactive(self):
        spec = importlib.util.spec_from_file_location("serial_diagnostics", ROOT / "scripts/serial-diagnostics.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for explicit in (False, True):
            stream = SimpleNamespace()
            observed = []
            stream.open = lambda: observed.append((stream.dtr, stream.rts, stream.dsrdtr, stream.rtscts))
            args = SimpleNamespace(port="COM9", baud=115200)
            if explicit:
                args.assert_dtr = True
            with patch.object(module.serial, "Serial", return_value=stream):
                self.assertIs(module.open_serial(args, module.time.time() + 5), stream)
            self.assertEqual(observed, [(explicit, False, False, False)])


if __name__ == "__main__":
    unittest.main()
