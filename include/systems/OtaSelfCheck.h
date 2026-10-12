/** OTA application confirmation window; not a complete hardware self-test. */
#ifndef FASTBEE_OTA_SELF_CHECK_H
#define FASTBEE_OTA_SELF_CHECK_H

#include <Arduino.h>
#include <stdint.h>

class OtaSelfCheck {
public:
    // Called before framework initialization. Repeated calls never re-arm a
    // failed/finished attempt or move the original confirmation deadline.
    static void begin();

    // NEW and PENDING_VERIFY images use the existing 90-second window.
    // Confirmation failures get at most three attempts, five seconds apart;
    // exhaustion remains pending and does not restart the device.
    static void handle();

    // Fatal initialization failure: capture evidence before the rollback SDK.
    // Only successful invalidation is followed by an explicit restart.
    // Failure returns to the caller's existing stopped-startup path.
    static void markInvalidAndRollback(const char* reason);

    static bool isPending();

private:
    static bool _begun;
    static bool _pending;
    static bool _confirmed;
    static bool _rollbackAttempted;
    static uint8_t _confirmationAttempts;
    static uint32_t _bootMs;
    static uint32_t _lastConfirmAttemptMs;
};

#endif
