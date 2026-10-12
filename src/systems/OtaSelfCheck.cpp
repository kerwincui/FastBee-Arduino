#include "systems/OtaSelfCheck.h"
#include "systems/RestartDiagnostics.h"
#include <esp_ota_ops.h>

namespace {
// Preserve the existing application-runtime window. Reaching it does not
// establish physical I/O, end-to-end network health or long-duration stability.
constexpr uint32_t OTA_SELF_CHECK_MS = 90000;
constexpr uint32_t OTA_CONFIRM_RETRY_MS = 5000;
constexpr uint8_t OTA_CONFIRM_MAX_ATTEMPTS = 3;

bool requiresConfirmation(esp_ota_img_states_t state) {
    return state == ESP_OTA_IMG_NEW || state == ESP_OTA_IMG_PENDING_VERIFY;
}
}

bool OtaSelfCheck::_begun = false;
bool OtaSelfCheck::_pending = false;
bool OtaSelfCheck::_confirmed = false;
bool OtaSelfCheck::_rollbackAttempted = false;
uint8_t OtaSelfCheck::_confirmationAttempts = 0;
uint32_t OtaSelfCheck::_bootMs = 0;
uint32_t OtaSelfCheck::_lastConfirmAttemptMs = 0;

void OtaSelfCheck::begin() {
    if (_begun || _rollbackAttempted) return;
    _begun = true;
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) {
        Serial.println("[OTA] Running partition unavailable; confirmation state unknown");
        return;
    }
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    const esp_err_t error = esp_ota_get_state_partition(running, &state);
    if (error != ESP_OK) {
        Serial.printf("[OTA] Cannot observe running image state: 0x%x\n", error);
        return;
    }
    if (!requiresConfirmation(state)) return;
    _pending = true;
    _bootMs = static_cast<uint32_t>(millis());
    Serial.printf("[OTA] Unconfirmed image (partition=%s state=%d): "
                  "application confirmation window %lus\n",
                  running->label, static_cast<int>(state),
                  static_cast<unsigned long>(OTA_SELF_CHECK_MS / 1000));
}

void OtaSelfCheck::handle() {
    if (!_pending || _confirmed || _rollbackAttempted ||
        _confirmationAttempts >= OTA_CONFIRM_MAX_ATTEMPTS) return;
    const uint32_t now = static_cast<uint32_t>(millis());
    if (static_cast<uint32_t>(now - _bootMs) < OTA_SELF_CHECK_MS) return;
    if (_confirmationAttempts &&
        static_cast<uint32_t>(now - _lastConfirmAttemptMs) < OTA_CONFIRM_RETRY_MS) return;

    ++_confirmationAttempts;
    _lastConfirmAttemptMs = now;
    const esp_err_t error = esp_ota_mark_app_valid_cancel_rollback();
    if (error == ESP_OK) {
        _confirmed = true;
        _pending = false;
        Serial.println("[OTA] Application image confirmed by SDK");
        return;
    }
    Serial.printf("[OTA] Confirmation attempt %u/%u failed: 0x%x; image remains unconfirmed\n",
                  static_cast<unsigned>(_confirmationAttempts),
                  static_cast<unsigned>(OTA_CONFIRM_MAX_ATTEMPTS), error);
    if (_confirmationAttempts == OTA_CONFIRM_MAX_ATTEMPTS) {
        Serial.println("[OTA] Confirmation retries exhausted; pending state retained, no automatic restart");
    }
}

void OtaSelfCheck::markInvalidAndRollback(const char* reason) {
    if (_confirmed || _rollbackAttempted) return;
    // A failed initialization must never later be confirmed by handle(), even
    // when state observation or the invalidation SDK reports an error.
    _rollbackAttempted = true;
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (!running) {
        Serial.println("[OTA] Fatal init: running partition unavailable; no rollback or restart attempted");
        return;
    }
    const esp_err_t stateError = esp_ota_get_state_partition(running, &state);
    if (stateError != ESP_OK) {
        Serial.printf("[OTA] Fatal init: state read failed 0x%x; no rollback or restart attempted\n", stateError);
        return;
    }
    if (!requiresConfirmation(state)) {
        _pending = false;
        Serial.printf("[OTA] Fatal init: image state %d is not awaiting confirmation; no rollback attempted\n",
                      static_cast<int>(state));
        return;
    }
    _pending = true;
    RestartDiagnostics::savePreRestartState(RestartReason::OTA_UPDATE,
        reason ? reason : "fatal initialization failure");
    Serial.flush();
    // The SDK variant used here does not reboot. Keep the diagnostic snapshot
    // before this call so evidence also survives an unexpected SDK reset.
    const esp_err_t error = esp_ota_mark_app_invalid_rollback();
    if (error != ESP_OK) {
        Serial.printf("[OTA] Rollback invalidation failed 0x%x; no automatic restart, image remains unconfirmed\n", error);
        return;
    }
    _pending = false;
    Serial.println("[OTA] Rollback invalidation accepted; restarting to the selected fallback image");
    delay(500);
    ESP.restart();
}

bool OtaSelfCheck::isPending() {
    return _pending && !_confirmed;
}
