#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace FastBee { namespace WeatherSchedule {
// No provider text, URLs or credentials are exposed through these reasons.
enum class Reason : uint8_t {
    None, Offline, Time, Memory, Ota, UnsupportedTransport, WorkerUnavailable,
    Transport, TlsConfiguration, Authentication, Location, RateLimit, Provider, Response
};
constexpr uint32_t MANUAL_INTERVAL_MS = 300000U;
constexpr size_t TLS_INTERNAL_BYTES = 65000U;
constexpr size_t WORKER_STACK_BYTES = 8192U;
constexpr size_t TLS_LARGEST_BLOCK_BYTES = 32768U;
// With the shared allocator active, TLS records and certificates live in PSRAM.
// These remain INTERNAL-only socket/control and scheduler reserves; PSRAM free
// is never substituted for DRAM, and worker stack consumption is accounted for.
constexpr size_t PSRAM_TLS_INTERNAL_BYTES = 24000U;
constexpr size_t PSRAM_TLS_LARGEST_BLOCK_BYTES = 16384U;
constexpr size_t PSRAM_WORKER_LARGEST_BLOCK_BYTES = 8192U;

inline size_t requiredFree(bool beforeWorker, bool tlsPsram) {
    return (tlsPsram ? PSRAM_TLS_INTERNAL_BYTES : TLS_INTERNAL_BYTES) +
        (beforeWorker ? WORKER_STACK_BYTES : 0U);
}
inline size_t requiredLargest(bool beforeWorker, bool tlsPsram) {
    // The 8 KiB internal stack has already been allocated inside the worker.
    // TLS records/certificates use PSRAM; HTTP RX is 4 KiB and socket buffers
    // are smaller than 8 KiB. Keep the independent total internal reserve.
    return tlsPsram ? (beforeWorker ? PSRAM_TLS_LARGEST_BLOCK_BYTES :
        PSRAM_WORKER_LARGEST_BLOCK_BYTES) : TLS_LARGEST_BLOCK_BYTES;
}

// Fixed-size diagnostics only: no request parameters or provider response text.
struct Diagnostics {
    const char* phase = "idle";
    uint32_t sampledAtMs = 0;
    uint32_t tickFree = 0, tickLargest = 0;
    uint32_t workerFree = 0, workerLargest = 0;
    uint32_t requestFree = 0, requestLargest = 0;
    uint32_t releasedFree = 0, releasedLargest = 0;
    uint32_t requiredFree = 0, requiredLargest = 0;
    uint32_t workerStackHighWaterBytes = 0;
};

inline bool due(uint32_t now, uint32_t deadline) {
    return !deadline || static_cast<int32_t>(now - deadline) >= 0;
}
inline uint32_t remaining(uint32_t now, uint32_t deadline) {
    return due(now, deadline) ? 0U : deadline - now;
}
inline uint32_t manualRemaining(uint32_t now, bool known, uint32_t updated,
                                bool seen, uint32_t requested) {
    uint32_t result = 0;
    if (known && now - updated < MANUAL_INTERVAL_MS)
        result = MANUAL_INTERVAL_MS - (now - updated);
    if (seen && now - requested < MANUAL_INTERVAL_MS)
        result = std::max(result, MANUAL_INTERVAL_MS - (now - requested));
    return result;
}
inline bool admissionBlocked(Reason reason) {
    return reason == Reason::Offline || reason == Reason::Time || reason == Reason::Memory ||
           reason == Reason::Ota || reason == Reason::UnsupportedTransport;
}
inline Reason admission(bool connected, bool timeReady, bool ota, size_t freeBytes,
                        size_t largestBlock, bool cellular, bool beforeWorker, bool tlsPsram=false) {
    if (!connected) return Reason::Offline;
    if (cellular) return Reason::UnsupportedTransport;
    if (!timeReady) return Reason::Time;
    if (ota) return Reason::Ota;
    if (freeBytes < requiredFree(beforeWorker,tlsPsram) ||
        largestBlock < requiredLargest(beforeWorker,tlsPsram)) return Reason::Memory;
    return Reason::None;
}
inline uint32_t failureDelay(uint8_t failures, Reason reason, uint32_t configured) {
    const uint8_t attempts = failures ? failures : 1U;
    if (admissionBlocked(reason) || reason == Reason::Transport || reason == Reason::WorkerUnavailable)
        return std::min<uint32_t>(30000U << std::min<uint8_t>(attempts - 1U, 4U), 300000U);
    return std::max<uint32_t>(configured, 60000U << std::min<uint8_t>(attempts, 6U));
}
inline const char* code(Reason reason) {
    switch (reason) {
    case Reason::None: return "none";
    case Reason::Offline: return "network_offline";
    case Reason::Time: return "time_unsynced";
    case Reason::Memory: return "memory_unavailable";
    case Reason::Ota: return "ota_paused";
    case Reason::UnsupportedTransport: return "transport_unsupported";
    case Reason::WorkerUnavailable: return "worker_unavailable";
    case Reason::Transport: return "transport_failed";
    case Reason::TlsConfiguration: return "tls_configuration";
    case Reason::Authentication: return "authentication";
    case Reason::Location: return "location_invalid";
    case Reason::RateLimit: return "rate_limited";
    case Reason::Provider: return "provider_error";
    case Reason::Response: return "response_invalid";
    }
    return "provider_error";
}
inline const char* message(Reason reason) {
    switch (reason) {
    case Reason::None: return "";
    case Reason::Offline: return "网络未连接，联网后自动更新";
    case Reason::Time: return "等待系统时间同步，校时后自动更新";
    case Reason::Memory: return "可用内存不足，内存恢复后自动更新";
    case Reason::Ota: return "升级期间暂停天气，升级结束后自动更新";
    case Reason::UnsupportedTransport: return "当前蜂窝网络不支持天气 HTTPS，请使用 Wi-Fi 或以太网";
    case Reason::WorkerUnavailable: return "天气任务暂不可用，将自动重试";
    case Reason::Transport: return "天气连接失败，请检查网络，将自动重试";
    case Reason::TlsConfiguration: return "天气证书配置不可用，请检查固件";
    case Reason::Authentication: return "心知天气 Key 无效或账号无实况权限";
    case Reason::Location: return "心知天气城市无效，请检查城市名称或坐标";
    case Reason::RateLimit: return "心知天气请求额度受限，请稍后再试";
    case Reason::Provider: return "心知天气服务暂不可用，将自动重试";
    case Reason::Response: return "天气响应不完整、无效或超过 8 KiB，将自动重试";
    }
    return "天气暂不可用";
}
}} // namespace FastBee::WeatherSchedule
