#pragma once
#include <time.h>
inline struct tm rtcSystemTime = [] {
    struct tm value{};value.tm_year=124;value.tm_mon=1;value.tm_mday=29;
    value.tm_hour=12;value.tm_min=34;value.tm_sec=56;value.tm_wday=4;return value;
}();
class TimeUtils {
public:
    static struct tm getTimeStruct(time_t = 0) { return rtcSystemTime; }
};
