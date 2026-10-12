#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecExecutor.h"
#include "core/RtcAction.h"
#include "core/RtcAccess.h"
#include "systems/LoggerSystem.h"
#if FASTBEE_ENABLE_DS1302
#include "utils/TimeUtils.h"
#include "utils/SystemTimeQuality.h"
#include "utils/SystemTimeZone.h"
#endif

bool PeriphExecExecutor::executeRtcAction(const ExecAction& action) {
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_RTC_CONTROL)) {
        LOGGER.warning("DS1302_ACTION_TYPE_INVALID");
        return false;
    }
    if (const char* error = FastBee::validateRtcAction(action)) {
        LOGGER.warning(error);
        return false;
    }
#if FASTBEE_ENABLE_DS1302
    FastBee::RtcAccess access(action.targetPeriphId);
    auto* rtc = access.driver();
    if (!rtc) { LOGGER.warning(access.error()); return false; }
    // A disconnected/unset clock must not pass a CH-bit-only readback.
    if (!rtc->readDateTime().isValid()) {
        LOGGER.warning("DS1302_INVALID_DATA");
        return false;
    }
    const bool running = action.actionValue == "start";
    if (rtc->isRunning() == running) return true;
    rtc->setRunning(running);
    if (rtc->isRunning() != running) {
        LOGGER.warning("DS1302_RUNNING_VERIFY_FAILED");
        return false;
    }
    return true;
#else
    return false;
#endif
}

bool PeriphExecExecutor::executeRtcReadAction(const ExecAction& action, ActionExecResult& result) {
    result = ActionExecResult{}; // Never reuse the previous successful value.
    const auto fail = [&](const char* reason) {
        result.remark = reason;
        LOGGER.warning(reason);
        return false;
    };
    if (!FastBee::captureLiteralReportIdentity(result, action.targetPeriphId.c_str()))
        return fail("DS1302_REPORT_IDENTITY_FAILED");
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ))
        return fail("DS1302_ACTION_TYPE_INVALID");
    if (const char* error = FastBee::validateRtcAction(action)) return fail(error);
#if FASTBEE_ENABLE_DS1302
    FastBee::RtcAccess access(action.targetPeriphId);
    auto* rtc = access.driver();
    if (!rtc) return fail(access.error());
    const DS1302ClockSnapshot snapshot = rtc->readClockSnapshot();
    if (!snapshot.valid) return fail("DS1302_INVALID_DATA");
    if (!snapshot.running) return fail("DS1302_HALTED");
    // DS1302 contains a calendar, not an offset/UTC or proof of synchronization.
    // Use the explicit peripheral ID as report ID; never infer an epoch/field.
    result.actualValue = snapshot.dateTime.toString();
    if (result.actualValue.isEmpty()) return fail("DS1302_REPORT_VALUE_FAILED");
    result.remark = "rtc_calendar_unzoned";
    result.success = true;
    return true;
#else
    return fail("DS1302_NOT_COMPILED");
#endif
}

bool PeriphExecExecutor::executeRtcSetAction(const ExecAction& action, ActionExecResult& result) {
    result = ActionExecResult{};
    result.targetPeriphId = action.targetPeriphId;
    const auto fail = [&](const char* reason) {
        result.remark = reason; LOGGER.warning(reason); return false;
    };
    if (action.actionType != static_cast<uint8_t>(ExecActionType::ACTION_RTC_SET))
        return fail("DS1302_ACTION_TYPE_INVALID");
    if (const char* error = FastBee::validateRtcAction(action)) return fail(error);
#if FASTBEE_ENABLE_DS1302
    FastBee::RtcAccess access(action.targetPeriphId);
    auto* rtc = access.driver();
    if (!rtc) return fail(access.error());
    // Copy only a currently tracked fresh source, never an arbitrary plausible
    // wall clock. Unauthenticated SNTP/HTTP/MQTT remains explicitly unverified.
    FastBee::TimeZoneGuard timezone;
    if (!timezone.held()) return fail("DS1302_TIMEZONE_BUSY");
    const auto timeStatus = FastBee::SystemTimeQuality::snapshot();
    if (!timeStatus.synced || !timeStatus.timeValid) return fail("DS1302_SYSTEM_TIME_UNSYNCED");
    const struct tm local = TimeUtils::getTimeStruct(static_cast<time_t>(timeStatus.epochMs / 1000));
    if (local.tm_year < 100 || local.tm_year > 199 || local.tm_mon < 0 || local.tm_mon > 11 ||
        local.tm_mday < 1 || local.tm_mday > 31 || local.tm_hour < 0 || local.tm_hour > 23 ||
        local.tm_min < 0 || local.tm_min > 59 || local.tm_sec < 0 || local.tm_sec > 59 ||
        local.tm_wday < 0 || local.tm_wday > 6) return fail("DS1302_SYSTEM_TIME_INVALID");
    const DS1302DateTime desired = {static_cast<uint8_t>(local.tm_year - 100),
        static_cast<uint8_t>(local.tm_mon + 1), static_cast<uint8_t>(local.tm_mday),
        static_cast<uint8_t>(local.tm_hour), static_cast<uint8_t>(local.tm_min),
        static_cast<uint8_t>(local.tm_sec), static_cast<uint8_t>(local.tm_wday == 0 ? 7 : local.tm_wday)};
    if (!desired.isValid()) return fail("DS1302_SYSTEM_TIME_INVALID");
    const auto before = rtc->readClockSnapshot();
    // An invalid/disconnected clock cannot establish its prior halt state.
    if (!before.valid) return fail("DS1302_INVALID_DATA");
    if (!FastBee::SystemTimeQuality::stillCurrent(timeStatus, 1000))
        return fail("DS1302_SYSTEM_TIME_CHANGED");
    if (!rtc->setDateTime(desired, before.running)) return fail("DS1302_WRITE_FAILED");
    const auto after = rtc->readClockSnapshot();
    // Calendar arithmetic only: these numbers are NOT published as UTC epochs.
    const uint32_t expected = desired.toUnixTimestamp();
    const uint32_t actual = after.dateTime.toUnixTimestamp();
    const bool nextDay = actual / 86400U != expected / 86400U;
    const uint8_t weekday = nextDay ? static_cast<uint8_t>(desired.dayOfWeek % 7 + 1) : desired.dayOfWeek;
    if (!after.valid || after.running != before.running || !after.mode24 ||
        actual < expected || actual - expected > (before.running ? 1U : 0U) ||
        after.dateTime.dayOfWeek != weekday) return fail("DS1302_WRITE_VERIFY_FAILED");
    result.actualValue = after.dateTime.toString();
    if (!FastBee::SystemTimeQuality::stillCurrent(timeStatus, 1000)) {
        result.actualValue = "";
        return fail("DS1302_SOURCE_CHANGED_AFTER_WRITE"); // No rollback claim.
    }
    result.remark = timeStatus.transportAuthenticated ? "rtc_copied_system_local_tls" : "rtc_copied_system_local_unverified";
    result.success = true;
    return true;
#else
    return fail("DS1302_NOT_COMPILED");
#endif
}
#endif
