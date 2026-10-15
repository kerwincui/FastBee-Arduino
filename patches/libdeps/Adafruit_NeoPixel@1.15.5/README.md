# NeoPixel 1.15.5 checked IDF5 transport

`esp.upstream.c` is the unmodified upstream source, retaining the original
Adafruit/Espressif copyright and Apache-2.0 notice. Its LF-normalized SHA256 is
`1f158b01845a3798f5cba7094c8fffba7effd96fe1903a379472ddc563ef1905`.

The build hook replaces only the IDF5 branch with `esp-idf5.c`. It first checks
`library.properties` is exactly 1.15.5 and verifies all three reviewed source
digests (`esp.c`, `Adafruit_NeoPixel.cpp`, `Adafruit_NeoPixel.h`). A rerun accepts
only the exact generated result; altered upstream, ABI or partial patch input
fails without overwrite. The hook preserves the full license and legacy IDF4
branch. A failed write attempts to restore the original and reports failures.

The pre-build hook explicitly adds the project `include` directory to the
environment inherited by library builders. It verifies the canonical
`include/peripherals/NeoPixelTransport.h` SHA256 before either configuring that
path or patching sources. No ABI header is copied into libdeps: firmware and the
patched dependency compile against the same archived input. The host transport
fixture obtains its include path from this hook, rather than supplying a hidden
extra path. The original r20 S3/C6 missing-header failures remain preserved;
this integration repair still requires a fresh actual firmware build.

Source references:

- [Adafruit NeoPixel 1.15.5 esp.c](https://github.com/adafruit/Adafruit_NeoPixel/blob/1.15.5/esp.c)
- [Arduino ESP32 3.3.8 RMT implementation](https://github.com/espressif/arduino-esp32/blob/3.3.8/cores/esp32/esp32-hal-rmt.c)
- [Arduino ESP32 3.3.8 RMT API](https://github.com/espressif/arduino-esp32/blob/3.3.8/cores/esp32/esp32-hal-rmt.h)

The original IDF5 implementation returns while holding `show_mutex` on failed
`rmtInit`, waits forever on writes and ignores write/deinit results. The patch
adds the project ABI in `include/peripherals/NeoPixelTransport.h`, releases the
mutex on every acquired path, verifies exact retained pin/bus consistency,
respects the latch gap, checks allocations and retains ambiguous resources.

SDK transmit false (including timeout), init failure, deinit failure and changed
ownership quarantine the backend until reboot. No later SDK completion permits
reuse; buffer/pin/object cannot be freed, rewritten or reassigned by FastBee.
The retained-pin query is atomic, allocation-free and makes no SDK call. A
periman pointer check does not provide an ownership generation lock against
unsupported external concurrent reassignment.

The 50 ms mutex wait and 100 ms transmit event wait are finite external waits.
Arduino 3.3.8 passes its timeout argument directly as RTOS ticks; the patch
converts explicitly and refuses a different SDK version at compile time. HAL
internal locks, initialization and cleanup are not proven hard realtime. This
is not a whole-call deadline guarantee. OOM before transmission and mutex busy
return errors without claiming output; they can retry without quarantine.
That retry policy applies to ordinary business writes. A failed project teardown
also retains a separate atomic pin reservation, including OOM/busy before RMT
starts. Its failed configuration stays visible; no later old-object "off" write
may target a pin reassigned after rollback. That state requires controlled reboot.

The patch is applied only by the existing serialized build hook, never by the
unit tests to shared libdeps. `test_neopixel_transport.py` uses a temporary copy
and SDK doubles. Physical color, power, RMT competition and timing still require
device acceptance (acceptance record `docs/system-improvement-neopixel-transport-20261004.md`
was removed in the 2026-10-13 docs cleanup; see git history).
