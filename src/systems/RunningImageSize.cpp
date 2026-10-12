#include "systems/RunningImageSize.h"

#include <esp_app_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <sdkconfig.h>
#include <limits>

namespace FastBee {
namespace {
constexpr uint32_t kMaxPartitionSize = 16u * 1024u * 1024u;
static_assert(sizeof(esp_image_header_t) == 24, "Image metadata read budget requires a 24-byte header");
static_assert(sizeof(esp_image_segment_header_t) == 8, "Image metadata read budget requires 8-byte segments");
static_assert(ESP_IMAGE_MAX_SEGMENTS <= 16, "Image metadata read budget permits at most 16 segments");

RunningImageSizeResult failure(RunningImageSizeError error, esp_err_t readError = ESP_OK) {
    RunningImageSizeResult result;
    result.error = error;
    result.readError = readError;
    return result;
}
} // namespace

const char* runningImageSizeErrorCode(RunningImageSizeError error) {
    switch (error) {
        case RunningImageSizeError::None: return "NONE";
        case RunningImageSizeError::NoRunningPartition: return "NO_RUNNING_PARTITION";
        case RunningImageSizeError::InvalidPartition: return "INVALID_RUNNING_PARTITION";
        case RunningImageSizeError::SignedImageUnsupported: return "SIGNED_IMAGE_SIZE_UNSUPPORTED";
        case RunningImageSizeError::HeaderReadFailed: return "IMAGE_HEADER_READ_FAILED";
        case RunningImageSizeError::InvalidHeader: return "IMAGE_HEADER_INVALID";
        case RunningImageSizeError::SegmentReadFailed: return "IMAGE_SEGMENT_READ_FAILED";
        case RunningImageSizeError::InvalidSegment: return "IMAGE_SEGMENT_INVALID";
        case RunningImageSizeError::TruncatedImage: return "IMAGE_PARTITION_TRUNCATED";
        case RunningImageSizeError::FooterReadFailed: return "IMAGE_FOOTER_READ_FAILED";
    }
    return "IMAGE_SIZE_UNKNOWN";
}

RunningImageSizeResult readRunningImageSize() {
    const esp_partition_t* partition = esp_ota_get_running_partition();
    if (!partition) return failure(RunningImageSizeError::NoRunningPartition);
    if (partition->type != ESP_PARTITION_TYPE_APP ||
        partition->size < sizeof(esp_image_header_t) || partition->size > kMaxPartitionSize ||
        partition->address > std::numeric_limits<uint32_t>::max() - partition->size) {
        return failure(RunningImageSizeError::InvalidPartition);
    }

#if CONFIG_SECURE_BOOT || CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT
    // Signature length/layout is outside this bounded unsigned metadata reader.
    return failure(RunningImageSizeError::SignedImageUnsupported);
#endif

    esp_image_header_t header{};
    esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
    if (err != ESP_OK) return failure(RunningImageSizeError::HeaderReadFailed, err);
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0 ||
        header.segment_count > ESP_IMAGE_MAX_SEGMENTS || header.hash_appended > 1 ||
        static_cast<uint16_t>(header.chip_id) != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return failure(RunningImageSizeError::InvalidHeader);
    }

    uint32_t offset = sizeof(header);
    for (uint8_t i = 0; i < header.segment_count; ++i) {
        if (sizeof(esp_image_segment_header_t) > partition->size - offset) {
            return failure(RunningImageSizeError::TruncatedImage);
        }
        esp_image_segment_header_t segment{};
        err = esp_partition_read(partition, offset, &segment, sizeof(segment));
        if (err != ESP_OK) return failure(RunningImageSizeError::SegmentReadFailed, err);
        offset += sizeof(segment);
        if ((segment.data_len & 3u) != 0 || segment.data_len >= kMaxPartitionSize ||
            segment.load_addr > std::numeric_limits<uint32_t>::max() - segment.data_len) {
            return failure(RunningImageSizeError::InvalidSegment);
        }
        if (segment.data_len > partition->size - offset) {
            return failure(RunningImageSizeError::TruncatedImage);
        }
        offset += segment.data_len;
    }

    // IDF pads the checksum byte to the next 16-byte boundary, followed by an
    // optional 32-byte simple hash. Reading their presence is not verification.
    const uint32_t tailSize = 16u - (offset & 15u) + (header.hash_appended ? 32u : 0u);
    if (tailSize > partition->size - offset) {
        return failure(RunningImageSizeError::TruncatedImage);
    }
    uint8_t tail[48];
    err = esp_partition_read(partition, offset, tail, tailSize);
    if (err != ESP_OK) return failure(RunningImageSizeError::FooterReadFailed, err);

    RunningImageSizeResult result;
    result.size = offset + tailSize;
    return result;
}

} // namespace FastBee
