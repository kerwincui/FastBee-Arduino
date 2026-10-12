"""Execute unmodified production transfer methods; no device or network access."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_panel_runtime_regressions import block

ROOT = Path(__file__).resolve().parents[2]


class ScreenI2CTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise RuntimeError("C++17 compiler required")
        cls.inputs = [ROOT / p for p in (
            "src/screen/Gt911TouchBackend.cpp", "src/screen/RgbDisplayBackend.cpp",
            "src/peripherals/IoExpanderProvider.cpp",
            "test/unit/cpp/screen_i2c_transport_harness.cpp")]
        cls.before = [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]
        gt, rgb, provider, harness = [p.read_text(encoding="utf-8") for p in cls.inputs]
        def methods(source, signatures):
            return "\n".join(signature + block(source, signature) for signature in signatures)
        harness = harness.replace("GT_METHODS", methods(gt, (
            "TwoWire* touchWireLocked()",
            "bool readRegister(uint16_t reg, uint8_t* data, size_t length)",
            "bool writeRegister(uint16_t reg, const uint8_t* data, size_t length)")))
        harness = harness.replace("RGB_METHODS", methods(rgb, (
            "TwoWire* controlWireLocked()", "uint8_t outputRegister() const",
            "bool readLocked(TwoWire* wire, uint8_t reg, uint8_t& value)",
            "bool writeLocked(TwoWire* wire, uint8_t reg, uint8_t value)",
            "bool writeRegister(uint8_t reg, uint8_t value)",
            "bool updateRegister(uint8_t reg, uint8_t mask, uint8_t value)")))
        for signature in ("IoExpanderResult IoExpanderProvider::writeByte(uint8_t value)",
                          "IoExpanderResult IoExpanderProvider::readByte(uint8_t& value)"):
            harness += "\n" + signature + block(provider, signature)
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-screen-transport-")
        cls.addClassCleanup(cls.temp.cleanup)
        base = Path(cls.temp.name)
        cpp = base / "transport.cpp"
        cpp.write_text(harness, encoding="utf-8")
        cls.exe = base / ("transport.exe" if os.name == "nt" else "transport")
        result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                 str(cpp), "-o", str(cls.exe)], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        if cls.before != [hashlib.sha256(p.read_bytes()).hexdigest() for p in cls.inputs]:
            raise AssertionError("Production or harness inputs changed while running")

    def check_mode(self, mode):
        result = subprocess.run([str(self.exe), mode], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=5)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"transport {mode} passed", result.stdout)


MODES = (
    "gt_pointer_short_first", "gt_pointer_short_second", "gt_pointer_nack",
    "gt_write_short_first", "gt_write_short_second", "gt_write_short_payload",
    "gt_write_nack", "gt_count_short", "gt_count_excess", "gt_missing",
    "gt_negative_first", "gt_negative_middle", "gt_negative_last", "gt_invalid_byte",
    "gt_negative_available", "gt_bounded_drain", "gt_drain_negative", "gt_success",
    "gt_invalid", "gt_busy", "gt_lost_lease", "gt_lost_address", "rgb_pointer_short", "rgb_pointer_nack",
    "rgb_write_short_first", "rgb_write_short_second", "rgb_write_nack",
    "rgb_count_short", "rgb_count_excess", "rgb_missing", "rgb_negative",
    "rgb_invalid_byte", "rgb_negative_available", "rgb_bounded_drain",
    "rgb_shadow_short", "rgb_rmw_missing", "rgb_success", "rgb_busy", "rgb_lost_lease",
    "rgb_lost_address",
    "provider_negative", "provider_invalid_byte", "provider_missing",
    "provider_short_write", "provider_success", "provider_busy", "provider_lost_lease")

for mode in MODES:
    setattr(ScreenI2CTransportTests, f"test_{mode}", lambda self, mode=mode: self.check_mode(mode))

if __name__ == "__main__":
    unittest.main()
