/**
 * @file OtaSelfCheck.cpp
 * @brief OTA 升级后自检与回滚控制实现
 * @date 2026-08-14
 */

#include "systems/OtaSelfCheck.h"
#include "systems/RestartDiagnostics.h"
#include <esp_ota_ops.h>

// 自检窗口：新固件稳定运行 90 秒后确认有效。
// 取值依据：框架完整初始化（FS/WiFi/MQTT/Web）实测 30-60 秒内完成，
// 90 秒窗口足以覆盖弱网首次连接；启动即崩溃的坏固件永远到不了确认点，
// 由 bootloader 在下次复位时自动回滚。
static const unsigned long OTA_SELF_CHECK_MS = 90000;

bool OtaSelfCheck::_pending = false;
bool OtaSelfCheck::_confirmed = false;
unsigned long OtaSelfCheck::_bootMs = 0;

void OtaSelfCheck::begin() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (!running) return;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;

    if (state == ESP_OTA_IMG_NEW) {
        _pending = true;
        _bootMs = millis();
        Serial.printf("[OTA] First boot after OTA (partition=%s state=PENDING): "
                      "will confirm valid after %lus stable run\n",
                      running->label, (unsigned long)(OTA_SELF_CHECK_MS / 1000));
    }
}

void OtaSelfCheck::handle() {
    if (!_pending || _confirmed) return;
    if (millis() - _bootMs < OTA_SELF_CHECK_MS) return;

    _confirmed = true;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        Serial.println("[OTA] Self-check passed, firmware marked valid (rollback cancelled)");
    } else {
        // bootloader 未启用回滚时返回 ERR_INVALID_STATE，属预期空转
        Serial.printf("[OTA] mark_app_valid returned 0x%x (rollback not enabled?)\n", err);
    }
    _pending = false;
}

void OtaSelfCheck::markInvalidAndRollback(const char* reason) {
    if (_confirmed) return;

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    bool pending = running &&
                   esp_ota_get_state_partition(running, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_NEW;

    Serial.printf("[OTA] Fatal init failure (%s), partition state=%d pending=%d\n",
                  reason ? reason : "unknown", (int)state, (int)pending);
    if (!pending) {
        Serial.println("[OTA] Not in post-OTA pending state, rollback not applicable");
        return;
    }

    Serial.println("[OTA] Marking firmware invalid, bootloader will roll back on reboot");
    Serial.flush();
    esp_err_t err = esp_ota_mark_app_invalid_rollback();
    if (err != ESP_OK) {
        Serial.printf("[OTA] mark_app_invalid failed (0x%x), rebooting anyway\n", err);
    }
    // 保存重启诊断快照，确保回滚重启后可通过 RestartDiagnostics 追溯原因
    RestartDiagnostics::savePreRestartState(RestartReason::OTA_UPDATE, reason);
    // mark invalid 成功后 ESP 会立即复位；失败时兜底手动复位
    delay(500);
    ESP.restart();
}

bool OtaSelfCheck::isPending() {
    return _pending && !_confirmed;
}
