#ifndef WIFI_RETRY_POLICY_H
#define WIFI_RETRY_POLICY_H

#include <stdint.h>

namespace WiFiRetryPolicy {

// Preserve one quick retry for transient handshake loss. Repeated failures use
// a bounded cooldown so an AP-side authentication throttle can expire.
static constexpr uint8_t AUTH_FAILURE_FAST_RETRIES = 1;
// The existing STA-only floor is two minutes and did not let the observed AP
// throttle expire. Start repeated-auth cooldown at five minutes, then cap at
// ten minutes so recovery remains bounded.
static constexpr uint32_t AUTH_COOLDOWN_BASE_MS = 300000UL;
static constexpr uint32_t AUTH_COOLDOWN_MAX_MS = 600000UL;

inline bool isAuthOrAssociationFailure(uint8_t reason) {
    switch (reason) {
        case 2:   // WIFI_REASON_AUTH_EXPIRE
        case 4:   // WIFI_REASON_ASSOC_EXPIRE / DISASSOC_DUE_TO_INACTIVITY
        case 5:   // WIFI_REASON_ASSOC_TOOMANY
        case 6:   // WIFI_REASON_NOT_AUTHED
        case 7:   // WIFI_REASON_NOT_ASSOCED
        case 9:   // WIFI_REASON_ASSOC_NOT_AUTHED
        case 14:  // WIFI_REASON_MIC_FAILURE
        case 15:  // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
        case 16:  // WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT
        case 17:  // WIFI_REASON_IE_IN_4WAY_DIFFERS
        case 18:  // WIFI_REASON_GROUP_CIPHER_INVALID
        case 19:  // WIFI_REASON_PAIRWISE_CIPHER_INVALID
        case 20:  // WIFI_REASON_AKMP_INVALID
        case 21:  // WIFI_REASON_UNSUPP_RSN_IE_VERSION
        case 22:  // WIFI_REASON_INVALID_RSN_IE_CAP
        case 23:  // WIFI_REASON_802_1X_AUTH_FAILED
        case 24:  // WIFI_REASON_CIPHER_SUITE_REJECTED
        case 29:  // WIFI_REASON_BAD_CIPHER_OR_AKM
        case 30:  // WIFI_REASON_NOT_AUTHORIZED_THIS_LOCATION
        case 49:  // WIFI_REASON_INVALID_PMKID
        case 50:  // WIFI_REASON_INVALID_MDE
        case 51:  // WIFI_REASON_INVALID_FTE
        case 202: // WIFI_REASON_AUTH_FAIL
        case 203: // WIFI_REASON_ASSOC_FAIL
        case 204: // WIFI_REASON_HANDSHAKE_TIMEOUT
            return true;
        default:
            return false;
    }
}

inline uint32_t authFailureCooldownMs(uint8_t consecutiveFailures) {
    if (consecutiveFailures <= AUTH_FAILURE_FAST_RETRIES) {
        return 0;
    }

    uint8_t exponent = consecutiveFailures - AUTH_FAILURE_FAST_RETRIES - 1;
    uint32_t cooldown = AUTH_COOLDOWN_BASE_MS;
    while (exponent-- > 0 && cooldown < AUTH_COOLDOWN_MAX_MS) {
        if (cooldown > AUTH_COOLDOWN_MAX_MS / 2) {
            return AUTH_COOLDOWN_MAX_MS;
        }
        cooldown *= 2;
    }
    return cooldown > AUTH_COOLDOWN_MAX_MS ? AUTH_COOLDOWN_MAX_MS : cooldown;
}

}  // namespace WiFiRetryPolicy

#endif // WIFI_RETRY_POLICY_H
