"""Execute the real bounded board catalog serializer without device access."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class BoardProfileCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            raise unittest.SkipTest("A C++17 compiler is required")
        cls.temp = tempfile.TemporaryDirectory(prefix="fastbee-board-catalog-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / ("catalog.exe" if os.name == "nt" else "catalog")
        command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-DUNIT_TEST",
                   "-DCONFIG_IDF_TARGET_ESP32C3=1", "-I" + str(ROOT / "test/unit/cpp/mocks"),
                   "-I" + str(ROOT / "test/mocks"), "-I" + str(ROOT / "include"),
                   str(ROOT / "test/unit/cpp/board_profile_catalog_harness.cpp"),
                   str(ROOT / "src/core/BoardProfile.cpp"),
                   str(ROOT / "src/network/BoardProfileCatalog.cpp"), "-o", str(cls.exe)]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace")
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_catalog(self, active=2, chip="ESP32-C3"):
        result = subprocess.run([str(self.exe), str(active), chip], capture_output=True,
                                text=True, encoding="utf-8", errors="replace", timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def test_complete_schema_and_c3_capabilities(self):
        payload = json.loads(self.run_catalog())
        self.assertTrue(payload["success"])
        data = payload["data"]
        self.assertEqual(data["selectionBasis"], "compiled-target-policy")
        profiles = data["profiles"]
        self.assertEqual(len(profiles), 7)
        self.assertEqual([p["profileCode"] for p in profiles], [0, 1, 3, 2, 2, 4, 5])
        for profile in profiles:
            self.assertEqual(profile["pinCount"], len(profile["pins"]))
            self.assertEqual(profile["id"], profile["key"])
            self.assertFalse(profile["hardwareVerified"])
            self.assertTrue(profile["requiresBoardWiringVerification"])
            self.assertEqual(len(profile["resources"]), 12)
            self.assertEqual(len(profile["defaultPins"]), 9)
            for pin in profile["pins"]:
                self.assertEqual(len(pin["capabilities"]), 8)
                self.assertTrue(all(isinstance(v, bool) for v in pin["capabilities"].values()))
                self.assertIsInstance(pin["risks"], list)
        c3 = profiles[2]
        self.assertEqual(c3["chip"], "ESP32-C3")
        self.assertEqual(c3["scope"], "chip-policy")
        self.assertEqual(c3["pinCount"], 22)
        self.assertEqual(c3["resources"]["rmtChannelCount"], 2)
        self.assertEqual(c3["resources"]["genericUartCount"], 1)
        self.assertIsNone(c3["defaultPins"]["onboardRgb"])
        pins = {p["gpio"]: p for p in c3["pins"]}
        for gpio in range(11, 18):
            self.assertIn("flash", pins[gpio]["risks"])
        for gpio in range(5):
            self.assertEqual(pins[gpio]["adc"]["unit"], 1)
            self.assertEqual(pins[gpio]["adc"]["channel"], gpio)
        self.assertNotIn("adc", pins[5])
        self.assertIn("usb_serial_jtag", pins[18]["risks"])

    def test_each_profile_has_exactly_one_active_identity(self):
        for choice in range(7):
            with self.subTest(choice=choice):
                data = json.loads(self.run_catalog(choice))["data"]
                active = [p for p in data["profiles"] if p["active"]]
                self.assertEqual(len(active), 1)
                self.assertEqual(active[0]["id"], data["activeProfileId"])
                self.assertEqual(active[0]["id"], data["profiles"][choice]["id"])

    def test_unknown_compiled_target_is_not_hardware_support(self):
        data = json.loads(self.run_catalog(-1))["data"]
        self.assertFalse(data["activeProfileSupported"])
        self.assertEqual(data["activeProfileId"], "")
        self.assertFalse(any(p["active"] for p in data["profiles"]))

    def test_classic_module_memory_policy_is_separate_from_carrier(self):
        profiles = {p["id"]: p for p in json.loads(self.run_catalog())["data"]["profiles"]}
        for key, psram in (("esp32-wroom", False), ("esp32-wrover", True)):
            profile = profiles[key]
            self.assertEqual(profile["scope"], "module-policy")
            self.assertFalse(profile["hardwareVerified"])
            pins = {p["gpio"]: p for p in profile["pins"]}
            for gpio in (16, 17):
                self.assertEqual(pins[gpio]["capabilities"]["output"], not psram)
                self.assertEqual("psram" in pins[gpio]["risks"], psram)
                self.assertEqual(pins[gpio]["policy"], "hard_forbidden" if psram else "recommended")
            for gpio in (34, 35, 36, 39):
                self.assertTrue(pins[gpio]["capabilities"]["input"])
                self.assertFalse(pins[gpio]["capabilities"]["output"])
                self.assertFalse(pins[gpio]["capabilities"]["pullUp"])
        self.assertEqual(profiles["puzhong-pz-esp32"]["scope"], "board-policy")

    def test_escaping_and_bounded_oversize_failure(self):
        chip = 'chip"' + chr(92) + chr(1) + chr(9) + chr(10)
        self.assertEqual(json.loads(self.run_catalog(chip=chip))["data"]["compiledChip"], chip)
        self.assertEqual(self.run_catalog(chip="x" * 1700), "overflow rejected")

    def test_http_authentication_and_allocation_failure_contract(self):
        route = (ROOT / "src/network/handlers/BoardProfileRouteHandler.cpp").read_text(encoding="utf-8")
        self.assertLess(route.index("ctx->requireAuth(request)"), route.index("new (std::nothrow)"))
        self.assertIn("catalog_.measureLength()", route)
        self.assertIn("catalog_.read(destination, capacity)", route)
        self.assertIn("delete response", route)
        self.assertIn("sendJsonError(request, 503", route)
        self.assertNotIn("JsonDocument", route)


if __name__ == "__main__":
    unittest.main()
