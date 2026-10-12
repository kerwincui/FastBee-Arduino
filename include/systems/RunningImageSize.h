#pragma once

#include <cstdint>
#include <esp_err.h>

namespace FastBee {

enum class RunningImageSizeError : uint8_t {
    None,
    NoRunningPartition,
    InvalidPartition,
    SignedImageUnsupported,
    HeaderReadFailed,
    InvalidHeader,
    SegmentReadFailed,
    InvalidSegment,
    TruncatedImage,
    FooterReadFailed
};

struct RunningImageSizeResult {
    uint32_t size = 0;
    RunningImageSizeError error = RunningImageSizeError::None;
    esp_err_t readError = ESP_OK;

    bool known() const { return error == RunningImageSizeError::None && size != 0; }
};

// Reads structural metadata only: at most 18 reads and 200 bytes, no heap or
// shared cache. Payload checksums, hashes and signatures are not verified here.
RunningImageSizeResult readRunningImageSize();
const char* runningImageSizeErrorCode(RunningImageSizeError error);

} // namespace FastBee
