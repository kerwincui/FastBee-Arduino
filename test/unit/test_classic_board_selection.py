"""Compile actual board selection, including the incompatible memory override."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ClassicBoardSelectionTests(unittest.TestCase):
    def test_build_target_selects_module_and_rejects_incompatible_board(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("C++17 compiler required")
        variants = [([], "esp32-wroom"), (["BOARD_HAS_PSRAM"], "esp32-wrover"),
                    (["FASTBEE_USE_PSRAM=1"], "esp32-wrover"),
                    (["FASTBEE_USE_PSRAM=0"], "esp32-wroom"),
                    (["FASTBEE_BOARD_PUZHONG_PZ_ESP32=1"], "puzhong-pz-esp32"),
                    (["FASTBEE_BOARD_PUZHONG_PZ_ESP32=1", "BOARD_HAS_PSRAM"], None)]
        with tempfile.TemporaryDirectory(prefix="fastbee-classic-profile-") as temporary:
            source = Path(temporary) / "selection.cpp"
            source.write_text('#include "core/BoardProfile.h"\n#include <iostream>\n'
                              'int main(){std::cout << FastBee::getCompiledBoardProfile().key;}', encoding="utf-8")
            for index, (flags, expected) in enumerate(variants):
                with self.subTest(flags=flags):
                    executable = Path(temporary) / (str(index) + (".exe" if os.name == "nt" else ""))
                    result = subprocess.run([compiler, "-std=c++17", "-DCONFIG_IDF_TARGET_ESP32=1",
                                             *["-D" + flag for flag in flags], "-I" + str(ROOT / "include"),
                                             str(source), str(ROOT / "src/core/BoardProfile.cpp"), "-o", str(executable)],
                                            capture_output=True, text=True, timeout=60)
                    if expected is None:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertIn("incompatible with classic PSRAM", result.stderr)
                    else:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                        output = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
                        self.assertEqual(output.returncode, 0, output.stderr)
                        self.assertEqual(output.stdout, expected)
