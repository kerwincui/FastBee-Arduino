"""Validate opt-in RGB experiments against the pinned Arduino SDK.

Only the RGB translation unit is rebuilt. The dependency functions already in
the vendor archives are placed in IRAM only for the IRAM experiment, as in
IDF's esp_lcd/linker.lf and esp_hw_support/dma/linker.lf. No SDK file is edited.
"""
import hashlib
from pathlib import Path

IDF_REVISION = "esp-idf: v5.5.4 735507283d"
PROFILE_HASHES = {
    "qio_opi": ("d2cfcee8cf531587826e82d56175ba71697412d63720ec750b56b2e34d48d1a3", "3b1bfae4ddbb8b805da59c5c48e9d2f603fd214aded75ba0a126d92135fd873f"),
    "dio_opi": ("d2cfcee8cf531587826e82d56175ba71697412d63720ec750b56b2e34d48d1a3", "3b1bfae4ddbb8b805da59c5c48e9d2f603fd214aded75ba0a126d92135fd873f"),
    "qio_qspi": ("82fe0db9fd6b0378a5b137bd7ad2b6dbfb2478edd5bd3e4ef6ffcb57793c27b8", "4bb60c17097f443c68f0625b8f754ddf654aea7733e7370101d22e0116235489"),
    "dio_qspi": ("82fe0db9fd6b0378a5b137bd7ad2b6dbfb2478edd5bd3e4ef6ffcb57793c27b8", "4bb60c17097f443c68f0625b8f754ddf654aea7733e7370101d22e0116235489"),
    "opi_opi": ("581836edab19beef65eee422ffbbfc9c85261683fcd266aed1ada12a91f762524", "c83162684125a86f42887a1112f1b2b0c3ef5a894ec78ead6b4007efdfc8132a"),
}
HAL_HASH = "f9e79cf99a731b6c9c039ee72ee2d96b15b7640664eaf4147a008ad2b182e98d"
RGB_HEADER_HASH = "7fb8382ad2fab5fd532461430311e306a82481a31807b901c1a4077cdc212849"
VENDOR_HASHES = {
    "esp_lcd_panel_rgb.c": "eab4844a88cf27e1156af7e64edb834e2aa137d28fa7bef406ee20238dc6ebee",
    "esp_lcd_common.h": "bbff1d2b72f1f6b255112557a45fef28ce3be5d652541f1203fdc403225b085c",
    "rgb_lcd_rotation_sw.h": "c623989c33f0d43015cd215f79490b95850f02212dece6e0941a23a591bcdf9b",
}
PLACEMENTS = (
    ("libesp_hw_support.a", "gdma", ("gdma_start", "gdma_reset")),
    ("libesp_hw_support.a", "gdma_link", ("gdma_link_get_head_addr",)),
    ("libhal.a", "gdma_hal_top", ("gdma_hal_start_with_desc", "gdma_hal_reset")),
    ("libhal.a", "gdma_hal_ahb_v1", ("gdma_ahb_hal_start_with_desc", "gdma_ahb_hal_reset")),
    ("libhal.a", "lcd_hal", ("lcd_hal_cal_pclk_freq",)),
    ("libhal.a", "hal_utils", ("hal_utils_calc_clk_div_frac_fast",)),
)


def require_hash(path, expected):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != expected:
        raise RuntimeError(f"RGB IRAM SDK/source mismatch: {path}; audit the new SDK before enabling")
    return data


def prepare_sections(original):
    code_anchor = "    _iram_text_start = ABSOLUTE(.);\n"
    data_anchor = "    _data_start = ABSOLUTE(.);\n"
    if original.count(code_anchor) != 1 or original.count(data_anchor) != 1:
        raise RuntimeError("RGB IRAM linker layout is not the audited SDK layout")
    code, data = [], []
    for archive, obj, symbols in PLACEMENTS:
        sections = " ".join(f".literal.{name} .text.{name}" for name in symbols)
        code.append(f"    *{archive}:{obj}.*({sections})\n")
        # Includes error/assert strings and compiler-generated function names.
        # Keep these inside the existing DRAM output section/heap accounting.
        data.append(f"    *{archive}:{obj}.*(.rodata .rodata.* .srodata .srodata.* .sdata2 .sdata2.*)\n")
    # lcd_ll clock-divider assertions are inlined into the local RGB ISR.
    # Their file/function/message strings belong to this project object, not
    # to libhal.a; omitting them leaves its failure path dependent on flash.
    data.append("    *RgbPanelDriver.c.o(.rodata .rodata.* .srodata .srodata.* .sdata2 .sdata2.*)\n")
    return original.replace(code_anchor, code_anchor + "    /* FastBee audited RGB ISR dependency closure */\n" + "".join(code), 1).replace(
        data_anchor, data_anchor + "    /* RGB ISR dependency constants, including error paths */\n" + "".join(data), 1)


def define_values(defines):
    result = {}
    for item in defines:
        if isinstance(item, (tuple, list)):
            result[str(item[0])] = str(item[1]) if len(item) > 1 else "1"
        else:
            name, _, value = str(item).partition("=")
            result[name] = value or "1"
    return result


def configure(environment):
    if environment.BoardConfig().get("build.mcu", "").lower() != "esp32s3":
        return
    defines = define_values(environment.get("CPPDEFINES", []))
    iram_override = defines.get("FASTBEE_RGB_ISR_IRAM_OVERRIDE", "0") == "1"
    restart_on_demand = defines.get("FASTBEE_RGB_RESTART_ON_DEMAND", "0") == "1"
    # Both experiments are explicit opt-ins. The restart-only experiment must
    # retain the original SDK linker script and interrupt/cache behavior.
    if not iram_override and not restart_on_demand:
        return
    screen_ui = defines.get("FASTBEE_ENABLE_SCREEN_UI", defines.get("FASTBEE_ENABLE_SCREEN_PANEL", "1"))
    if screen_ui != "1" or defines.get("FASTBEE_USE_PSRAM", "0") != "1":
        return
    sdk = Path(environment.PioPlatform().get_package_dir("framework-arduinoespressif32-libs"))
    if IDF_REVISION not in (sdk / "versions.txt").read_text():
        raise RuntimeError("RGB override requires the audited IDF revision")
    candidates = []
    for entry in environment.get("LIBPATH", []):
        directory = Path(environment.subst(str(entry))).resolve()
        if directory.parent == (sdk / "esp32s3").resolve() and (directory / "sections.ld").is_file():
            candidates.append(directory)
    candidates = list(dict.fromkeys(candidates))
    if len(candidates) != 1 or candidates[0].name not in PROFILE_HASHES:
        raise RuntimeError("RGB override cannot identify the active SDK memory profile")
    profile = candidates[0]
    section_hash, gdma_hash = PROFILE_HASHES[profile.name]
    original = require_hash(profile / "sections.ld", section_hash).decode("utf-8")
    require_hash(profile / "libesp_hw_support.a", gdma_hash)
    require_hash(sdk / "esp32s3/lib/libhal.a", HAL_HASH)
    require_hash(sdk / "esp32s3/include/esp_lcd/rgb/include/esp_lcd_panel_rgb.h", RGB_HEADER_HASH)
    vendor = Path(environment.subst("$PROJECT_DIR")) / "vendor/esp-idf-rgb-735507283d"
    for name, expected in VENDOR_HASHES.items():
        require_hash(vendor / name, expected)
    if not iram_override:
        print(f"RGB: restart-on-demand experiment; original SDK linker/IRQ ({profile.name})")
        return
    config = (profile / "include/sdkconfig.h").read_text()
    for required in ("CONFIG_GDMA_OBJ_DRAM_SAFE", "CONFIG_GDMA_ISR_HANDLER_IN_IRAM"):
        if f"#define {required} 1" not in config:
            raise RuntimeError(f"RGB IRAM override needs {required}")
    destination = Path(environment.subst("$BUILD_DIR")) / "rgb-iram"
    destination.mkdir(parents=True, exist_ok=True)
    script = destination / "sections.ld"
    patched = prepare_sections(original)
    if not script.exists() or script.read_text() != patched:
        script.write_text(patched, encoding="utf-8", newline="\n")
    # Arduino passes -T sections.ld. GNU ld searches -L directories in order;
    # the existing SDK script remains untouched and available to other builds.
    environment.Prepend(LIBPATH=[str(destination)])
    environment.Depends("$BUILD_DIR/${PROGNAME}.elf", str(script))
    print(f"RGB: cache-safe driver + audited IRAM dependencies ({profile.name})")


if "Import" in globals():
    Import("env")  # type: ignore[name-defined]
    configure(env)  # type: ignore[name-defined]
