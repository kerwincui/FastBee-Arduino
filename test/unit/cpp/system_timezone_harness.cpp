#include "utils/SystemTimeZone.h"
#include "utils/SystemTimePlatform.h"
#include <cassert>
#include <cstring>
#include <string>
#include <thread>
using Zone = FastBee::SystemTimeZone;
using Guard = FastBee::TimeZoneGuard;
static std::string active = "CST-8";
static bool absent = false, writeOk = true, sdkDrift = false, reentry = false;
static unsigned writes = 0, starts = 0;
static bool formatOk = true;
static int64_t formattedEpoch = -1;
const char* FastBee::SystemTimePlatform::readTimezone() { return absent ? nullptr : active.c_str(); }
bool FastBee::SystemTimePlatform::writeTimezone(const char* value) {
    ++writes;
    if (!writeOk) return false;
    // Emulate replacement/free of the borrowed getenv buffer.
    std::string copy(value); active.assign(400, 'x'); active = copy; absent = false;
    return true;
}
void FastBee::SystemTimePlatform::configureSntp(const char* value, const char* first,
                                               const char*, const char*) {
    ++starts; assert(first && *first);
    const std::string expected(value);
    assert(expected == active);
    assert(!Zone::setPosix("UTC0")); // Guard spans the SDK's own TZ mutation.
    reentry = true;
    active.assign(400, 'y');
    assert(std::string(value) == expected); // Stable non-borrowed argument.
    active = sdkDrift ? "UTC0" : expected;
}
bool FastBee::SystemTimePlatform::formatLocal(int64_t epoch, char* output, size_t capacity) {
    Guard competingWriter; assert(!competingWriter.held()); formattedEpoch=epoch;
    assert(capacity >= 20); std::strcpy(output,"2026-10-02 18:00:00");
    return formatOk;
}
int main(int argc, char** argv) {
    assert(argc == 2); const std::string mode(argv[1]);
    char output[Zone::CAPACITY]{};
    if (mode == "preserve") {
        assert(Zone::startSntp(nullptr, "fixture.invalid"));
        assert(active == "CST-8" && writes == 1 && starts == 1 && reentry);
    } else if (mode == "set") {
        assert(Zone::setPosix("UTC-5:30") && active == "UTC-5:30");
        assert(Zone::copyActive(output, sizeof(output)) && std::string(output) == active);
    } else if (mode == "default") {
        absent = true; assert(Zone::copyActive(output, sizeof(output)) && std::string(output) == "UTC0");
        assert(Zone::startSntp(nullptr, "fixture.invalid") && active == "UTC0");
    } else if (mode == "invalid") {
        assert(!Zone::setPosix(nullptr) && !Zone::setPosix(""));
        std::string longZone(Zone::CAPACITY, 'x');
        assert(!Zone::setPosix(longZone.c_str()) && !Zone::startSntp(longZone.c_str(), "fixture.invalid"));
        assert(!Zone::startSntp("UTC0", nullptr) && !Zone::startSntp("UTC0", ""));
        assert(writes == 0 && starts == 0 && active == "CST-8");
    } else if (mode == "busy") {
        { Guard held; assert(held.held());
          std::thread other([&] {
              assert(!Zone::setPosix("UTC0") && !Zone::startSntp(nullptr, "fixture.invalid"));
              output[0] = 'x'; assert(!Zone::copyActive(output, sizeof(output)) && output[0] == 0);
          }); other.join(); assert(writes == 0 && starts == 0);
        }
        assert(Zone::setPosix("UTC0"));
    } else if (mode == "write_failure") {
        writeOk = false;
        assert(!Zone::setPosix("UTC0") && !Zone::startSntp("UTC0", "fixture.invalid"));
        assert(starts == 0 && active == "CST-8");
        writeOk = true; assert(Zone::setPosix("UTC0"));
    } else if (mode == "sdk_drift") {
        sdkDrift = true; assert(!Zone::startSntp(nullptr, "fixture.invalid"));
        assert(starts == 1 && active == "UTC0"); // No rollback/network-stop claim.
    } else if (mode == "buffer") {
        output[0] = 'x'; assert(!Zone::copyActive(output, 2) && output[0] == 0);
        assert(!Zone::copyActive(nullptr, 3) && !Zone::copyActive(output, 0));
        active.assign(Zone::CAPACITY - 1, 'x');
        assert(Zone::copyActive(output, sizeof(output)) && std::strlen(output) == active.size());
    } else if (mode == "local_snapshot") {
        const auto local=Zone::localSnapshot(123456);
        assert(local.timezoneAvailable && local.calendarAvailable && formattedEpoch==123456);
        assert(std::string(local.timezone)=="CST-8" && std::string(local.calendar)=="2026-10-02 18:00:00");
    } else if (mode == "local_invalid") {
        const auto local=Zone::localSnapshot(-1);
        assert(local.timezoneAvailable && !local.calendarAvailable && local.calendar[0]==0 && formattedEpoch==-1);
    } else if (mode == "local_busy") {
        Guard writer; assert(writer.held());const auto local=Zone::localSnapshot(123456);
        assert(!local.timezoneAvailable && !local.calendarAvailable && local.calendar[0]==0 && formattedEpoch==-1);
    } else if (mode == "local_failed") {
        formatOk=false;const auto local=Zone::localSnapshot(123456);
        assert(local.timezoneAvailable && !local.calendarAvailable && local.calendar[0]==0);
    } else assert(false);
}
