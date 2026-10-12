#pragma once
#include "core/PeripheralExecution.h"
#include <atomic>
#include <ctime>

namespace FastBee { namespace DailyTriggerState {

// One check can release rulesMutex during admission. Serialize that entire
// scan/admit/commit window without waiting or holding a configuration lock.
class TickGuard {
public:
    explicit TickGuard(std::atomic_flag& flag) : flag_(flag), held_(!flag.test_and_set(std::memory_order_acquire)) {}
    ~TickGuard() { if (held_) flag_.clear(std::memory_order_release); }
    bool held() const { return held_; }
    TickGuard(const TickGuard&) = delete;
    TickGuard& operator=(const TickGuard&) = delete;
private:
    std::atomic_flag& flag_;
    const bool held_;
};

// Sortable local date, deliberately independent of UTC offset, source trust or
// sync state. Keep the existing year>=2000 calendar policy; bound arithmetic.
inline uint32_t dateKey(const tm& value) {
    if (value.tm_year < 100 || value.tm_year > 8099 || value.tm_mon < 0 || value.tm_mon > 11 ||
        value.tm_hour < 0 || value.tm_hour > 23 || value.tm_min < 0 || value.tm_min > 59 ||
        value.tm_sec < 0 || value.tm_sec > 60) return 0;
    const uint32_t year=static_cast<uint32_t>(value.tm_year+1900);
    constexpr uint8_t days[]={31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap=year%4==0 && (year%100!=0 || year%400==0);
    const unsigned maxDay=days[value.tm_mon]+(value.tm_mon==1 && leap?1U:0U);
    if (value.tm_mday < 1 || static_cast<unsigned>(value.tm_mday) > maxDay) return 0;
    return year*10000U+static_cast<uint32_t>(value.tm_mon+1)*100U+static_cast<uint32_t>(value.tm_mday);
}

inline bool isDaily(const ExecTrigger& value) {
    return (value.triggerType==static_cast<uint8_t>(ExecTriggerType::TIMER_TRIGGER) ||
            value.triggerType==static_cast<uint8_t>(ExecTriggerType::POLL_TRIGGER)) &&
        value.timerMode==1 && value._cachedHour>=0 && value._cachedHour<=23 &&
        value._cachedMinute>=0 && value._cachedMinute<=59;
}

inline bool sameSchedule(const ExecTrigger& left,const ExecTrigger& right) {
    return isDaily(left) && isDaily(right) && left._cachedHour==right._cachedHour &&
        left._cachedMinute==right._cachedMinute;
}

// Caller already sanitized the new trigger. Client-supplied runtime fields
// never seed the watermark. Reordering or renaming the same HH:mm preserves it.
inline void inherit(ExecTrigger& next,const std::vector<ExecTrigger>& previous) {
    next.lastCalendarDay=0;
    for (const auto& old : previous) {
        if (sameSchedule(next,old) && old.lastCalendarDay>next.lastCalendarDay)
            next.lastCalendarDay=old.lastCalendarDay;
    }
}

// Duplicate OR clauses at the same HH:mm denote the same rule occurrence.
// Consume only after successful admission, never on an observed/rejected due.
inline void accept(std::vector<ExecTrigger>& triggers,const ExecTrigger& matched,uint32_t day) {
    if (!day) return;
    for (auto& trigger : triggers) {
        if (sameSchedule(trigger,matched) && day>trigger.lastCalendarDay)
            trigger.lastCalendarDay=day;
    }
}
} } // namespace FastBee::DailyTriggerState
