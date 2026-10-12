// FastBee patch: checked NeoPixel transport, revision 1.
// Adapted from Adafruit NeoPixel 1.15.5 esp.c (Lucian Copeland / Adafruit,
// Espressif Systems). The original esp.c Apache-2.0 notice is retained by
// the patch hook. See esp.upstream.c for the unmodified licensed source.
#include "peripherals/NeoPixelTransport.h"
#include <esp_arduino_version.h>
#include <esp32-hal-periman.h>
#include <esp_heap_caps.h>
#include <limits.h>

// 3.3.8 rmtWrite passes timeout_ms directly to xEventGroupWaitBits as ticks.
// An SDK upgrade requires reviewing these semantics before enabling output.
#if ESP_ARDUINO_VERSION != ESP_ARDUINO_VERSION_VAL(3, 3, 8)
#error "FastBee NeoPixel transport requires reviewed Arduino ESP32 3.3.8"
#endif

static SemaphoreHandle_t show_mutex = NULL;
static portMUX_TYPE fastbee_neo_init_mux = portMUX_INITIALIZER_UNLOCKED;
static rmt_data_t *fastbee_neo_data = NULL;
static uint32_t fastbee_neo_capacity = 0;
static int fastbee_neo_pin = -1;
static void *fastbee_neo_bus = NULL;
static bool fastbee_neo_quarantined = false;
static int fastbee_neo_retained_pin = -1;

bool fastbeeNeoPixelPinRetained(uint8_t pin) {
  return __atomic_load_n(&fastbee_neo_retained_pin, __ATOMIC_ACQUIRE) == pin;
}

static void fastbeeNeoPixelQuarantine(void) {
  fastbee_neo_quarantined = true;
  __atomic_store_n(&fastbee_neo_retained_pin, fastbee_neo_pin, __ATOMIC_RELEASE);
}

// Allocate outside the short critical section; publish once, then retain the
// mutex for process lifetime. No device/diagnostic path initializes it by query.
static SemaphoreHandle_t fastbeeNeoPixelMutex(void) {
  portENTER_CRITICAL(&fastbee_neo_init_mux);
  SemaphoreHandle_t handle = show_mutex;
  portEXIT_CRITICAL(&fastbee_neo_init_mux);
  if (handle) return handle;
  SemaphoreHandle_t candidate = xSemaphoreCreateMutex();
  if (!candidate) return NULL;
  portENTER_CRITICAL(&fastbee_neo_init_mux);
  if (!show_mutex) { show_mutex = candidate; candidate = NULL; }
  handle = show_mutex;
  portEXIT_CRITICAL(&fastbee_neo_init_mux);
  if (candidate) vSemaphoreDelete(candidate);
  return handle;
}

static FastBeeNeoPixelResult fastbeeNeoPixelDetachLocked(void) {
  if (fastbee_neo_quarantined) return FASTBEE_NEO_QUARANTINED;
  if (fastbee_neo_pin >= 0) {
    // Refuse to detach a pin that periman now assigns to another owner. This
    // is a consistency check under our mutex, not a global periman generation
    // lock; external concurrent pin reassignment remains unsupported.
    if (!fastbee_neo_bus ||
        perimanGetPinBus(fastbee_neo_pin, ESP32_BUS_TYPE_RMT_TX) != fastbee_neo_bus) {
      fastbeeNeoPixelQuarantine();
      return FASTBEE_NEO_OWNER_CHANGED;
    }
    if (!rmtDeinit(fastbee_neo_pin)) {
      fastbeeNeoPixelQuarantine();
      return FASTBEE_NEO_RELEASE_FAILED;
    }
    fastbee_neo_pin = -1;
    fastbee_neo_bus = NULL;
  }
  return FASTBEE_NEO_OK;
}

FastBeeNeoPixelResult fastbeeNeoPixelRelease(uint8_t pin) {
  SemaphoreHandle_t handle = fastbeeNeoPixelMutex();
  if (!handle) return FASTBEE_NEO_NO_MEMORY;
  if (xSemaphoreTake(handle, pdMS_TO_TICKS(50)) != pdTRUE) return FASTBEE_NEO_BUSY;
  FastBeeNeoPixelResult result;
  if (fastbee_neo_quarantined) result = FASTBEE_NEO_QUARANTINED;
  else if (fastbee_neo_pin >= 0 && fastbee_neo_pin != pin) result = FASTBEE_NEO_INVALID;
  else result = fastbeeNeoPixelDetachLocked();
  if (result == FASTBEE_NEO_OK) {
    free(fastbee_neo_data);
    fastbee_neo_data = NULL;
    fastbee_neo_capacity = 0;
  }
  xSemaphoreGive(handle);
  return result;
}

FastBeeNeoPixelResult fastbeeNeoPixelWrite(uint8_t pin, const uint8_t *pixels,
                                         uint32_t numBytes, bool is800KHz) {
  if (!pixels || !numBytes || numBytes > UINT32_MAX / 8U ||
      (size_t)numBytes > SIZE_MAX / (8U * sizeof(rmt_data_t))) return FASTBEE_NEO_INVALID;
  SemaphoreHandle_t handle = fastbeeNeoPixelMutex();
  if (!handle) return FASTBEE_NEO_NO_MEMORY;
  if (xSemaphoreTake(handle, pdMS_TO_TICKS(50)) != pdTRUE) return FASTBEE_NEO_BUSY;
  FastBeeNeoPixelResult result = FASTBEE_NEO_OK;
  uint32_t required = numBytes * 8U;
  if (fastbee_neo_quarantined) { result = FASTBEE_NEO_QUARANTINED; goto done; }
  if (fastbee_neo_pin >= 0 &&
      perimanGetPinBus(fastbee_neo_pin, ESP32_BUS_TYPE_RMT_TX) != fastbee_neo_bus) {
    fastbeeNeoPixelQuarantine();
    result = FASTBEE_NEO_OWNER_CHANGED; goto done;
  }
  if (pin != fastbee_neo_pin) {
    result = fastbeeNeoPixelDetachLocked();
    if (result != FASTBEE_NEO_OK) goto done;
  }
  if (required > fastbee_neo_capacity) {
    rmt_data_t *replacement = (rmt_data_t *)heap_caps_malloc(
        (size_t)required * sizeof(rmt_data_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!replacement) { result = FASTBEE_NEO_NO_MEMORY; goto done; }
    // Earlier successful rmtWrite completed. A failed write would quarantine
    // before this path and preserve the original buffer until reboot.
    free(fastbee_neo_data);
    fastbee_neo_data = replacement;
    fastbee_neo_capacity = required;
  }
  if (fastbee_neo_pin < 0) {
    fastbee_neo_pin = pin; // Preserve attempted ownership if SDK cleanup fails.
    if (!rmtInit(pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000)) {
      // SDK init error cleanup is not observable here. Do not retry/free/reuse.
      fastbeeNeoPixelQuarantine();
      result = FASTBEE_NEO_INIT_FAILED; goto done;
    }
    fastbee_neo_bus = perimanGetPinBus(pin, ESP32_BUS_TYPE_RMT_TX);
    if (!fastbee_neo_bus) {
      fastbeeNeoPixelQuarantine();
      result = FASTBEE_NEO_OWNER_CHANGED; goto done;
    }
  }
  for (uint32_t b = 0, i = 0; b < numBytes; ++b) {
    for (unsigned bit = 0; bit < 8; ++bit, ++i) {
      bool one = (pixels[b] & (1U << (7U - bit))) != 0;
      fastbee_neo_data[i].level0 = 1;
      fastbee_neo_data[i].level1 = 0;
      fastbee_neo_data[i].duration0 = is800KHz ? (one ? 8 : 4) : (one ? 12 : 5);
      fastbee_neo_data[i].duration1 = is800KHz ? (one ? 4 : 8) : (one ? 13 : 20);
    }
  }
  // Checked callers bypass Adafruit show(); preserve its >=300us latch gap.
  delayMicroseconds(300);
  if (!rmtWrite(pin, fastbee_neo_data, required, pdMS_TO_TICKS(100))) {
    // false includes rejected transmit AND timeout with a still-active DMA/
    // encoder. Even later completion does not clear quarantine. Never free,
    // rewrite, resize, switch pins, or detach these retained resources here.
    fastbeeNeoPixelQuarantine();
    result = FASTBEE_NEO_WRITE_FAILED;
  }
done:
  xSemaphoreGive(handle);
  return result;
}

void espShow(uint16_t pin, uint8_t *pixels, uint32_t numBytes, uint8_t is800KHz) {
  // Compatibility only: FastBee uses the checked API and guards destruction.
  // Match Adafruit_NeoPixel.cpp's C ABI without truncating an invalid pin.
  if (pin > UINT8_MAX) return;
  if (!numBytes) (void)fastbeeNeoPixelRelease(pin);
  else (void)fastbeeNeoPixelWrite(pin, pixels, numBytes, is800KHz);
}

void espInit(void) { (void)fastbeeNeoPixelMutex(); }
