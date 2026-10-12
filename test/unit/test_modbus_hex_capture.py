"""Compile real Modbus TX/RX capture blocks without worker-stack formatters."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
RAW_MARKER = "OneShotError ModbusHandler::_executeModbusTransactionRaw("


def capture_blocks(source):
    """Extract just the two actual heap-gated capture blocks from Raw."""
    if source.count(RAW_MARKER) != 1:
        raise AssertionError("Expected one real Raw transaction function")
    start = source.index(RAW_MARKER)
    body = source[start:source.index("\n}", start) + 2]
    result, cursor = [], 0
    while True:
        start = body.find("if (heapSafe) {", cursor)
        if start < 0:
            break
        depth = 0
        for end in range(body.index("{", start), len(body)):
            depth += (body[end] == "{") - (body[end] == "}")
            if depth == 0:
                break
        if depth:
            raise AssertionError("Unbalanced capture block")
        result.append(body[start:end + 1])
        cursor = end + 1
    if (len(result) != 2 or "_lastTxHex" not in result[0]
            or "_lastRxHex" not in result[1]):
        raise AssertionError("Actual TX/RX capture boundary differs")
    return result


class ModbusHexCaptureTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # CXX is one executable path, not a shell command. An invalid explicit
        # choice must fail rather than silently bypassing the selected compiler.
        explicit = os.environ.get("CXX")
        cls.compiler = (shutil.which(explicit) if explicit else
                        shutil.which("g++") or shutil.which("clang++"))
        if not cls.compiler:
            raise AssertionError("C++17 compiler required (CXX, g++, or clang++)")
        source = (ROOT / "src/protocols/ModbusHandler.cpp").read_text(encoding="utf-8")
        cls.tx, cls.rx = capture_blocks(source)
        cls.template = (ROOT / "test/unit/cpp/modbus_hex_capture_harness.cpp.in").read_text(
            encoding="utf-8")
        if cls.template.count("@TX_BLOCK@") != 1 or cls.template.count("@RX_BLOCK@") != 1:
            raise AssertionError("Harness injection tokens differ")

    def compile_and_check(self, tx, rx, *, failure=None, compile_rejected=False):
        cpp = self.template.replace("@TX_BLOCK@", tx).replace("@RX_BLOCK@", rx)
        # Inherit TEMP/TMP, PATH and the caller's process ownership boundary.
        # No platform build, board, serial port or artifact directory is needed.
        with tempfile.TemporaryDirectory(prefix="fastbee-modbus-hex-") as directory:
            base = Path(directory)
            source = base / "capture.cpp"
            executable = base / ("capture.exe" if os.name == "nt" else "capture")
            source.write_text(cpp, encoding="utf-8")
            compiled = subprocess.run(
                [self.compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 str(source), "-o", str(executable)],
                capture_output=True, text=True, timeout=40)
            diagnostics = compiled.stdout + compiled.stderr
            if compile_rejected:
                self.assertNotEqual(compiled.returncode, 0, diagnostics)
                self.assertIn("FASTBEE_FORBIDDEN_snprintf", compiled.stderr, diagnostics)
                return
            self.assertEqual(compiled.returncode, 0, diagnostics)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 1 if failure else 0, ran.stdout + ran.stderr)
            if failure:
                self.assertIn("FAIL " + failure, ran.stderr)
            else:
                self.assertIn("PASS checks=", ran.stdout)

    def test_actual_capture_preserves_bytes_spacing_and_heap_guards(self):
        self.compile_and_check(self.tx, self.rx)

    def test_oracle_rejects_capture_regressions(self):
        mutants = [
            ("lowercase", self.tx.replace("0123456789ABCDEF", "0123456789abcdef"),
             self.rx, "tx resets rx and exact bytes"),
            ("missing-separator", self.tx,
             self.rx.replace("if (i > 0) _lastRxHex += ' ';", ""),
             "rx keeps tx and exact bytes"),
            ("reversed-heap-guard", self.tx.replace("if (heapSafe)", "if (!heapSafe)", 1),
             self.rx, "tx heapSafe false keeps both"),
            ("truncated-256-byte-response", self.tx,
             self.rx.replace("i < respIdx", "i < respIdx && i < 255", 1),
             "rx keeps tx and exact bytes"),
        ]
        for name, tx, rx, failure in mutants:
            with self.subTest(mutation=name):
                self.assertNotEqual((tx, rx), (self.tx, self.rx), "Mutation target missing")
                self.compile_and_check(tx, rx, failure=failure)

    def test_original_formatter_is_rejected_at_compile_time(self):
        # Restore only the historic formatter in the extracted code. The
        # independent oracle remains outside the harness macro poison boundary.
        old_blocks = []
        for block, data in ((self.tx, "request"), (self.rx, "respBuf")):
            old, count = re.subn(
                r"const char hex\[3\] = \{[^}]*\};",
                'char hex[4]; snprintf(hex, sizeof(hex), "%02X", ' + data + '[i]);',
                block)
            self.assertEqual(count, 1, "Expected one formatter-free byte encoding")
            self.assertNotEqual(old, block)
            old_blocks.append(old)
        self.compile_and_check(*old_blocks, compile_rejected=True)


if __name__ == "__main__":
    unittest.main()
