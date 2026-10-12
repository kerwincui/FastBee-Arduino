#pragma once
#include "utils/CheckedText.h"
#include <cstdint>

namespace FastBee {
template<class Text>
bool validMqttTopicText(const Text& topic, bool publish) {
    return topic.length() > 0 && topic.length() <= UINT16_MAX &&
        !std::memchr(topic.c_str(), '\0', topic.length()) &&
        (!publish || (!std::memchr(topic.c_str(), '+', topic.length()) &&
                      !std::memchr(topic.c_str(), '#', topic.length())));
}

template<class Text>
Text buildMqttTopicPath(const Text& topic, bool autoPrefix, bool ota,
                       const Text& device, const Text& product, const Text& extra) {
    constexpr size_t limit = UINT16_MAX;
    Text expandedDevice, resolved;
    // Preserve the original two-pass order, including product placeholders
    // inside device text, without recursively expanding an inserted token.
    if (!replaceCheckedText(topic, "{deviceNum}", device.c_str(), device.length(), expandedDevice, limit) ||
        !replaceCheckedText(expandedDevice, "{productId}", product.c_str(), product.length(), resolved, limit))
        return Text();
    if (!autoPrefix) return resolved;
    Text result;
    const auto append = [&](const char* bytes, size_t size) {
        return appendCheckedText(result, bytes, size, limit);
    };
    if (ota) {
        // Already rooted OTA paths must not be prefixed a second time.
        if (device.length() && resolved.length() >= device.length() + 1U &&
            resolved.c_str()[0] == '/' &&
            std::memcmp(resolved.c_str() + 1, device.c_str(), device.length()) == 0 &&
            (resolved.length() == device.length() + 1U || resolved.c_str()[device.length() + 1U] == '/'))
            return resolved;
        if (device.length() && (!append("/", 1) || !append(device.c_str(), device.length()))) return Text();
        if ((!resolved.length() || resolved.c_str()[0] != '/') && !append("/", 1)) return Text();
    } else {
        if (!append(extra.c_str(), extra.length())) return Text();
        if (device.length()) {
            if (product.length() && (!append("/", 1) || !append(product.c_str(), product.length()))) return Text();
            if (!append("/", 1) || !append(device.c_str(), device.length())) return Text();
        }
    }
    if (!append(resolved.c_str(), resolved.length())) return Text();
    return result;
}
} // namespace FastBee
