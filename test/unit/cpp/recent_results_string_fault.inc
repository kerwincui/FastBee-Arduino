
// Fault seam for String storage only. Production still uses ESP32 WString.
struct RecentTextFault {
    long failAt = -1;
    bool persistent = false, partial = false, lie = false;
    size_t calls = 0, failures = 0;
    void reset(long at = -1, bool persist = false, bool truncate = false, bool reportSuccess = false) {
        failAt = at; persistent = persist; partial = truncate; lie = reportSuccess;
        calls = failures = 0;
    }
    bool fail() {
        const size_t index = calls++;
        const bool rejected = failAt >= 0 && (index == static_cast<size_t>(failAt) ||
            (persistent && index > static_cast<size_t>(failAt)));
        if (rejected) ++failures;
        return rejected;
    }
};
inline RecentTextFault recentTextFault;

// These seams are only installed in this harness's temporary Arduino.h.
// Keep global mocks unchanged, and observe ownership after the route returns.
#include <set>
#include <new>
#include <utility>
inline std::set<const std::string*> recentStringValues;
struct RecentStringProbe {
    const std::string* value;
    explicit RecentStringProbe(const std::string* input) : value(input) { recentStringValues.insert(value); }
    ~RecentStringProbe() { recentStringValues.erase(value); }
    RecentStringProbe(const RecentStringProbe&) = delete;
};
inline size_t recentLiveStringBytes() {
    size_t bytes = 0;
    for (const auto* value : recentStringValues) bytes += value->size();
    return bytes;
}
inline bool recentBodyCopyActive = false, recentBodyCopyFailure = false;
inline unsigned recentBodyCopies = 0, recentBodyCopyFailures = 0;
inline bool recentFooterFailure = false, recentArrayFailure = false, recentResponseFailure = false;
inline unsigned recentLiveArrays = 0, recentLiveResponses = 0;
inline long recentMoveFailAt = -1;
inline size_t recentMoveCalls = 0;
