"""Guard the scoped SDK relink, including rejection of unreviewed SDK changes."""
import importlib.util
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("rgb_iram_build", ROOT / "scripts/prepare-rgb-iram.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RgbIramBuildTests(unittest.TestCase):
    def test_vendor_is_exact_audited_source(self):
        for name, digest in MODULE.VENDOR_HASHES.items():
            MODULE.require_hash(ROOT / "vendor/esp-idf-rgb-735507283d" / name, digest)

    def test_unreviewed_source_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "unknown.ld"
            source.write_text("changed SDK")
            with self.assertRaisesRegex(RuntimeError, "SDK/source mismatch"):
                MODULE.require_hash(source, "0" * 64)

    def test_layout_must_match_once(self):
        for text in ("", "    _iram_text_start = ABSOLUTE(.);\n" * 2 + "    _data_start = ABSOLUTE(.);\n"):
            with self.assertRaisesRegex(RuntimeError, "linker layout"):
                MODULE.prepare_sections(text)

    def test_complete_indirect_hal_chain_precedes_flash_and_heap(self):
        original = "    _iram_text_start = ABSOLUTE(.);\nIRAM_END\n    _data_start = ABSOLUTE(.);\nDATA_END\nFLASH\n"
        result = MODULE.prepare_sections(original)
        for _, _, names in MODULE.PLACEMENTS:
            for name in names:
                self.assertLess(result.index(f".text.{name}"), result.index("IRAM_END"))
                self.assertLess(result.index(f".literal.{name}"), result.index("IRAM_END"))
        self.assertEqual(sum(len(names) for _, _, names in MODULE.PLACEMENTS), 9)
        self.assertLess(result.index(".rodata.*"), result.index("DATA_END"))
        self.assertIn(".srodata.* .sdata2 .sdata2.*", result)
        self.assertLess(result.index("*RgbPanelDriver.c.o(.rodata"), result.index("DATA_END"))

    def test_other_targets_and_no_psram_do_not_touch_sdk(self):
        class Environment(dict):
            def BoardConfig(self):
                return {"build.mcu": self["mcu"]}
            def PioPlatform(self):
                raise AssertionError("must not resolve SDK")
        for mcu, defines in (("esp32", []), ("esp32c3", []), ("esp32c6", []),
                             ("esp32s3", [("FASTBEE_USE_PSRAM", 0)]),
                             ("esp32s3", [("FASTBEE_USE_PSRAM", 2)]),
                             ("esp32s3", [("FASTBEE_USE_PSRAM", 1), ("FASTBEE_ENABLE_SCREEN_UI", 0)]),
                             ("esp32s3", [("FASTBEE_USE_PSRAM", 1), ("FASTBEE_ENABLE_SCREEN_UI", 2)])):
            for flag in ("FASTBEE_RGB_ISR_IRAM_OVERRIDE", "FASTBEE_RGB_RESTART_ON_DEMAND"):
                MODULE.configure(Environment(mcu=mcu, CPPDEFINES=defines + [(flag, 1)]))

    def test_experimental_override_defaults_off(self):
        class Environment(dict):
            def BoardConfig(self): return {"build.mcu": "esp32s3"}
            def PioPlatform(self): raise AssertionError("disabled override must not resolve SDK")
        for defines in ([], [("FASTBEE_RGB_ISR_IRAM_OVERRIDE", 0)],
                        [("FASTBEE_RGB_RESTART_ON_DEMAND", 0)],
                        [("FASTBEE_RGB_RESTART_ON_DEMAND", 2)],
                        [("FASTBEE_RGB_ISR_IRAM_OVERRIDE", 2)]):
            MODULE.configure(Environment(CPPDEFINES=[("FASTBEE_USE_PSRAM", 1)] + defines))

    def test_panel_disabled_fallback_does_not_touch_sdk(self):
        class Environment(dict):
            def BoardConfig(self): return {"build.mcu": "esp32s3"}
            def PioPlatform(self): raise AssertionError("disabled panel must not resolve SDK")
        for flag in ("FASTBEE_RGB_ISR_IRAM_OVERRIDE", "FASTBEE_RGB_RESTART_ON_DEMAND"):
            MODULE.configure(Environment(CPPDEFINES=[("FASTBEE_USE_PSRAM", 1),
                             (flag, 1), ("FASTBEE_ENABLE_SCREEN_PANEL", 0)]))

    def test_installed_sdk_generates_only_build_local_script(self):
        sdk = Path.home() / ".platformio/packages/framework-arduinoespressif32-libs"
        if not sdk.exists():
            self.skipTest("optional installed SDK audit")
        with tempfile.TemporaryDirectory() as tmp:
            class Environment(dict):
                def BoardConfig(self): return {"build.mcu": "esp32s3"}
                def PioPlatform(self): return self
                def get_package_dir(self, _): return str(sdk)
                def subst(self, value): return value.replace("$PROJECT_DIR", str(ROOT)).replace("$BUILD_DIR", tmp)
                def Prepend(self, **kwargs): self["prepared"] = kwargs
                def Depends(self, *args): self["dependency"] = args
            profile = sdk / "esp32s3/qio_opi"
            before = (profile / "sections.ld").read_bytes()
            env = Environment(CPPDEFINES=[("FASTBEE_USE_PSRAM", 1), ("FASTBEE_RGB_ISR_IRAM_OVERRIDE", 1)], LIBPATH=[str(profile)])
            MODULE.configure(env)
            self.assertEqual(before, (profile / "sections.ld").read_bytes())
            script = Path(tmp) / "rgb-iram/sections.ld"
            self.assertTrue(script.exists())
            self.assertIn("gdma_hal_start_with_desc", script.read_text())
            self.assertEqual(env["prepared"]["LIBPATH"], [str(script.parent)])

    def test_restart_only_validates_hashes_without_linker_mutation(self):
        sdk = Path.home() / ".platformio/packages/framework-arduinoespressif32-libs"
        if not sdk.exists():
            self.skipTest("optional installed SDK audit")
        with tempfile.TemporaryDirectory() as tmp:
            class Environment(dict):
                def BoardConfig(self): return {"build.mcu": "esp32s3"}
                def PioPlatform(self): return self
                def get_package_dir(self, _): return str(sdk)
                def subst(self, value): return value.replace("$PROJECT_DIR", str(ROOT)).replace("$BUILD_DIR", tmp)
                def Prepend(self, **kwargs): raise AssertionError("restart only must retain SDK linker")
                def Depends(self, *args): raise AssertionError("restart only must not add linker dependency")
            profile = sdk / "esp32s3/qio_opi"
            before = (profile / "sections.ld").read_bytes()
            env = Environment(CPPDEFINES=[("FASTBEE_USE_PSRAM", 1),
                ("FASTBEE_RGB_ISR_IRAM_OVERRIDE", 0), ("FASTBEE_RGB_RESTART_ON_DEMAND", 1)],
                LIBPATH=[str(profile)])
            with patch.object(MODULE, "require_hash", wraps=MODULE.require_hash) as checked:
                MODULE.configure(env)
            self.assertEqual(checked.call_count, 4 + len(MODULE.VENDOR_HASHES))
            self.assertEqual(list(Path(tmp).iterdir()), [])
            self.assertEqual(env["LIBPATH"], [str(profile)])
            self.assertEqual(before, (profile / "sections.ld").read_bytes())
            with patch.object(MODULE, "require_hash", side_effect=RuntimeError("SDK/source mismatch")):
                with self.assertRaisesRegex(RuntimeError, "SDK/source mismatch"):
                    MODULE.configure(env)

    def test_actual_c_wrapper_gates_and_local_sdk_macros(self):
        packages = Path.home() / ".platformio/packages"
        sdk = packages / "framework-arduinoespressif32-libs/esp32s3"
        toolchain = packages / "toolchain-xtensa-esp-elf/bin"
        gcc = toolchain / "xtensa-esp32s3-elf-gcc.exe"
        if not gcc.exists() or not sdk.exists():
            self.skipTest("optional installed Xtensa C compiler audit")
        base = [str(gcc), "-Os", "-DESP_PLATFORM", "-DSOC_MMU_PAGE_SIZE=CONFIG_MMU_PAGE_SIZE",
                "-DSOC_XTAL_FREQ_MHZ=CONFIG_XTAL_FREQ"]
        base += shlex.split((sdk / "flags/c_flags").read_text())
        base += ["-iprefix", str(sdk / "include") + "/"]
        base += shlex.split((sdk / "flags/includes").read_text())
        base += ["-I", str(sdk / "qio_opi/include"), "-I", str(ROOT / "include")]
        source = str(ROOT / "src/screen/RgbPanelDriver.c")
        sdk_config = (sdk / "qio_opi/include/sdkconfig.h").read_text()
        sdk_iram_enabled = "#define CONFIG_LCD_RGB_ISR_IRAM_SAFE 1" in sdk_config
        with tempfile.TemporaryDirectory() as tmp:
            # (IRAM override, restart override, PSRAM, UI, local driver active)
            for iram, restart, psram, ui, active in (
                    (None, None, 1, 1, False), (0, 0, 1, 1, False),
                    (2, 2, 1, 1, False), (0, 1, 1, 1, True),
                    (1, 0, 1, 1, True), (1, 1, 1, 1, True),
                    (0, 1, 0, 1, False), (0, 1, 1, 2, False)):
                flags = [f"-DFASTBEE_USE_PSRAM={psram}", f"-DFASTBEE_ENABLE_SCREEN_UI={ui}"]
                if iram is not None: flags.append(f"-DFASTBEE_RGB_ISR_IRAM_OVERRIDE={iram}")
                if restart is not None: flags.append(f"-DFASTBEE_RGB_RESTART_ON_DEMAND={restart}")
                output = Path(tmp) / "driver.o"
                subprocess.run(base + flags + ["-c", source, "-o", str(output)], check=True, capture_output=True)
                symbols = subprocess.check_output([str(toolchain / "xtensa-esp32s3-elf-nm.exe"),
                                                   "-S", str(output)], text=True)
                self.assertEqual(" T esp_lcd_new_rgb_panel" in symbols, active)
                macros = subprocess.check_output(base + flags + ["-dM", "-E", source], text=True)
                self.assertEqual("#define CONFIG_LCD_RGB_ISR_IRAM_SAFE 1" in macros,
                                 active and iram == 1 or sdk_iram_enabled)
                self.assertEqual("#define CONFIG_LCD_RGB_RESTART_IN_VSYNC 0" in macros,
                                 active and restart == 1)


if __name__ == "__main__":
    unittest.main()
