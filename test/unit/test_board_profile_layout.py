"""Compare actual board tables/catalog with the immutable pre-layout implementation."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "test/unit/cpp/board_profile_layout_before"

class BoardProfileLayoutTests(unittest.TestCase):
    def test_all_pin_fields_catalog_bytes_and_compiled_selections(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        old_header = (FIXTURE / "BoardProfile.h.inc").read_text(encoding="utf-8")
        old_source = (FIXTURE / "BoardProfile.cpp.inc").read_text(encoding="utf-8")
        self.assertEqual(hashlib.sha256(old_header.encode("utf-8")).hexdigest(), "3673b7300130d84bbb974b778c15fc014b0bb7011ec702f9eab8159db1e9732f")
        self.assertEqual(hashlib.sha256(old_source.encode("utf-8")).hexdigest(), "dea71c41586bd53e7c64a29529f547f0bdd6533a69f907b9c4f348559c70ee08")
        variants = (
            ("classic", ["CONFIG_IDF_TARGET_ESP32=1"]),
            ("classic-psram", ["CONFIG_IDF_TARGET_ESP32=1", "BOARD_HAS_PSRAM=1"]),
            ("classic-puzhong", ["CONFIG_IDF_TARGET_ESP32=1", "FASTBEE_BOARD_PUZHONG_PZ_ESP32=1"]),
            ("c3", ["CONFIG_IDF_TARGET_ESP32C3=1"]),
            ("c6", ["CONFIG_IDF_TARGET_ESP32C6=1"]),
            ("s3-quad", ["CONFIG_IDF_TARGET_ESP32S3=1"]),
            ("s3-octal", ["CONFIG_IDF_TARGET_ESP32S3=1", "CONFIG_SPIRAM_MODE_OCT=1"]),
        )
        with tempfile.TemporaryDirectory(prefix="fastbee-board-layout-") as temporary:
            temp = Path(temporary)
            def legacy(text):
                return text.replace("FastBee", "LegacyFastBee").replace(
                    '"core/BoardProfile.h"', '"legacy_board_profile.h"').replace(
                    '"network/BoardProfileCatalog.h"', '"legacy_board_catalog.h"')
            (temp / "legacy_board_profile.h").write_text(legacy(old_header), encoding="utf-8")
            (temp / "legacy_board_profile.cpp").write_text(legacy(old_source), encoding="utf-8")
            (temp / "legacy_board_catalog.h").write_text(legacy(
                (ROOT / "include/network/BoardProfileCatalog.h").read_text(encoding="utf-8")), encoding="utf-8")
            (temp / "legacy_board_catalog.cpp").write_text(legacy(
                (ROOT / "src/network/BoardProfileCatalog.cpp").read_text(encoding="utf-8")), encoding="utf-8")
            for name, flags in variants:
                with self.subTest(target=name):
                    exe = temp / (name + (".exe" if os.name == "nt" else ""))
                    command = [
                        compiler, "-std=c++17", "-Wall", "-Wextra",
                        "-I" + str(temp), "-I" + str(ROOT / "include"),
                        *["-D" + flag for flag in flags],
                        str(ROOT / "test/unit/cpp/board_profile_layout_harness.cpp"),
                        str(ROOT / "src/core/BoardProfile.cpp"),
                        str(ROOT / "src/network/BoardProfileCatalog.cpp"),
                        str(temp / "legacy_board_profile.cpp"),
                        str(temp / "legacy_board_catalog.cpp"), "-o", str(exe),
                    ]
                    built = subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                                           errors="replace", timeout=90, cwd=ROOT)
                    self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                    ran = subprocess.run([str(exe)], capture_output=True, text=True, encoding="utf-8",
                                         errors="replace", timeout=90, cwd=ROOT)
                    self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
