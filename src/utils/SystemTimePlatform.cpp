#include "utils/SystemTimePlatform.h"
#include <Arduino.h>
#include <cstdlib>
#include <atomic>
#include <limits>
#include <sys/time.h>
#include <esp_timer.h>
#include <esp_sntp.h>

namespace FastBee {
namespace {
std::atomic<SystemTimePlatform::Observer> timeObserver{nullptr};
int64_t milliseconds(const timeval* value) {
    if (!value || value->tv_sec < 0 || value->tv_usec < 0 || value->tv_usec >= 1000000 ||
        static_cast<uint64_t>(value->tv_sec) > static_cast<uint64_t>((INT64_MAX - 999) / 1000)) return 0;
    return static_cast<int64_t>(value->tv_sec) * 1000 + value->tv_usec / 1000;
}
void observed(timeval* value) {
    const auto observer = timeObserver.load(std::memory_order_acquire);
    if (observer) observer(milliseconds(value));
}
}
uint64_t SystemTimePlatform::monotonicMs() {
    const int64_t sampled = esp_timer_get_time();
    return monotonicFromMicroseconds(sampled);
}
int64_t SystemTimePlatform::wallMs() {
    timeval value{};
    return gettimeofday(&value, nullptr) == 0 ? milliseconds(&value) : 0;
}
bool SystemTimePlatform::writeMs(int64_t epochMs) {
    if (epochMs < 0 || epochMs / 1000 > std::numeric_limits<time_t>::max()) return false;
    timeval value{static_cast<time_t>(epochMs / 1000),
                  static_cast<suseconds_t>((epochMs % 1000) * 1000)};
    return settimeofday(&value, nullptr) == 0;
}
void SystemTimePlatform::installObserver(Observer observer) {
    timeObserver.store(observer, std::memory_order_release);
    sntp_set_time_sync_notification_cb(observed);
}
const char* SystemTimePlatform::readTimezone() { return std::getenv("TZ"); }
bool SystemTimePlatform::writeTimezone(const char* timezone) {
    if (setenv("TZ", timezone, 1) != 0) return false;
    tzset();
    return true;
}
void SystemTimePlatform::configureSntp(const char* timezone, const char* server1,
                                      const char* server2, const char* server3) {
    configTzTime(timezone, server1, server2, server3);
}
bool SystemTimePlatform::formatLocal(int64_t epochMs, char* output, size_t capacity) {
    if (!output || !capacity) return false;
    output[0] = '\0';
    if (epochMs < 0 || epochMs / 1000 > std::numeric_limits<time_t>::max()) return false;
    const time_t epoch = static_cast<time_t>(epochMs / 1000);
    struct tm local{};
    if (!localtime_r(&epoch, &local)) return false;
    if (strftime(output, capacity, "%Y-%m-%d %H:%M:%S", &local) == 0) {
        output[0] = '\0'; return false;
    }
    return true;
}
} // namespace FastBee
