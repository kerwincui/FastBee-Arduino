# Pinned ESP-IDF RGB driver

## Restart policy experiment, disabled by default

`-DFASTBEE_RGB_RESTART_ON_DEMAND=1` recompiles only the matching official RGB
driver with `CONFIG_LCD_RGB_RESTART_IN_VSYNC=0`. Keep
`FASTBEE_RGB_ISR_IRAM_OVERRIDE=0` for this experiment: it preserves the SDK's
interrupt allocation and original linker script, with no GDMA/code/constants
relocation. The build hook still checks the exact SDK and vendor hashes.

The official Kconfig defaults unconditional VBlank restart to off. Its driver
comments explain that a late VBlank restart can itself cause a one-frame shift.
This experiment tests that mechanism; it has **not passed hardware acceptance**.
If actual DMA starvation occurs, a directly streamed PSRAM framebuffer can
remain shifted: this mode has no automatic underrun detection on ESP32-S3.
The public `esp_lcd_rgb_panel_restart()` API remains available for an explicit
recovery request. Do not claim that this flag proves a flicker fix.

Both flags require the exact value `1`. With both absent/zero, no local driver
or linker override is enabled. Both paths additionally require ESP32-S3,
`FASTBEE_USE_PSRAM=1`, and `FASTBEE_ENABLE_SCREEN_UI=1` (falling back to
`FASTBEE_ENABLE_SCREEN_PANEL`, then `1`, when undefined).

## IRAM experiment, disabled by default

This override has **not passed hardware acceptance**. The first candidate
(`c64fb53d...`) repeatedly restarted before RGB initialization; the cause is
still under investigation. Passing compilation and IRAM placement checks is
not evidence that this candidate is safe to deploy or resolves flicker.

Only an explicit build definition
`-DFASTBEE_RGB_ISR_IRAM_OVERRIDE=1` enables both the local driver and private
linker override. Keep this definition absent (or set to `0`) for releases.
Screen UI, ESP32-S3, and configured PSRAM are also required. The hook and
translation unit share these gates. Retest boot, display, touch, Web access,
cache-off activity, and sustained operation before changing the default.

These three unmodified Apache-2.0 files come from Espressif ESP-IDF commit
`735507283d` (v5.5.4), matching the Arduino binary SDK pinned by this project:

- `components/esp_lcd/rgb/esp_lcd_panel_rgb.c`
- `components/esp_lcd/rgb/rgb_lcd_rotation_sw.h`
- `components/esp_lcd/priv_include/esp_lcd_common.h`

Upstream: https://github.com/espressif/esp-idf/tree/735507283d/components/esp_lcd

`src/screen/RgbPanelDriver.c` locally enables the official cache-safe LCD/GDMA
interrupt allocation and internal panel state. The build hook strictly checks
the SDK/source hashes and creates a build-local `sections.ld` that places the
complete GDMA control/HAL and PCLK helper call chain in IRAM, including error
path constants in DRAM. The installed SDK is never modified. This applies only
to S3 builds with screen UI and configured PSRAM. Other targets use the SDK.

Keep direct EDMA (no PSRAM-to-bounce copying during cache-off), the existing
PCLK and rendering behavior. Enabling the ISR flag alone is unsafe because the
binary SDK's GDMA control functions otherwise remain in flash. Any SDK upgrade
requires auditing and updating the hashes, dependencies, and post-link map.
