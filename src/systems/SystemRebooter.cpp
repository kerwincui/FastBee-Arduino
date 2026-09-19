/**
 * @file SystemRebooter.cpp
 * @brief 统一延迟重启实现
 */
#include "systems/SystemRebooter.h"
#include "systems/RestartDiagnostics.h"
#include "systems/ConfigStorage.h"
#include "core/FeatureFlags.h"
#include "core/PeripheralManager.h"
#if FASTBEE_ENABLE_POINT_HISTORY
#include "core/PointHistory.h"
#endif
#if FASTBEE_ENABLE_SCREEN_PANEL
#include "core/ScreenPanelManager.h"
#endif
#include <ESP.h>

// ========== 静态成员初始化 ==========
bool SystemRebooter::_scheduled = false;
unsigned long SystemRebooter::_rebootAt = 0;
char SystemRebooter::_reason[48] = {0};
RestartReason SystemRebooter::_restartReason = RestartReason::CONFIG_CHANGE;

void SystemRebooter::scheduleReboot(const char* reason, unsigned long delayMs, RestartReason restartReason) {
    if (reason) {
        strncpy(_reason, reason, sizeof(_reason) - 1);
        _reason[sizeof(_reason) - 1] = '\0';
    } else {
        strncpy(_reason, "Reboot", sizeof(_reason) - 1);
        _reason[sizeof(_reason) - 1] = '\0';
    }
    _rebootAt = millis() + delayMs;
    _restartReason = restartReason;
    _scheduled = true;

    Serial.printf("[REBOOTER] Scheduled reboot in %lums [%s]: %s\n",
                  (unsigned long)delayMs,
                  RestartDiagnostics::getRestartReasonString(restartReason),
                  _reason);
}

void SystemRebooter::scheduleConfigReboot(const char* reason, unsigned long delayMs) {
    scheduleReboot(reason, delayMs, RestartReason::CONFIG_CHANGE);
}

void SystemRebooter::cancelScheduledReboot() {
    if (_scheduled) {
        Serial.printf("[REBOOTER] Cancelled scheduled reboot: %s\n", _reason);
        _scheduled = false;
        _rebootAt = 0;
        _reason[0] = '\0';
        _restartReason = RestartReason::UNKNOWN;
    }
}

void SystemRebooter::update() {
    if (!_scheduled) return;

    unsigned long now = millis();
    // 处理 millis() 溢出：如果 _rebootAt 已过期（now >= _rebootAt 或溢出场景）
    if (now >= _rebootAt || (_rebootAt > now + 60000UL)) {
        _scheduled = false;  // 先清除标志，防止中断重入

#if FASTBEE_ENABLE_POINT_HISTORY
        // Prepare on the system loop, never in the touch callback/UI task.
        // A failed user-requested panel restart remains on screen for a second,
        // explicit confirmation; health/OTA restarts must remain available.
        bool skipHistorySave = false;
#if FASTBEE_ENABLE_SCREEN_PANEL
        const bool panelRestart = strcmp(_reason, "Screen panel restart") == 0 ||
                                  strcmp(_reason, "Panel restart without history") == 0;
        if (strcmp(_reason, "Panel restart without history") == 0) {
            skipHistorySave = ScreenPanelManager::instance().consumeRestartWithoutHistorySave();
        }
#endif
        if (!skipHistorySave && !FastBee::PointHistory::instance().flushNow(2000)) {
            Serial.println("[REBOOTER] Recent point history could not be saved");
#if FASTBEE_ENABLE_SCREEN_PANEL
            if (panelRestart) {
                ScreenPanelManager::instance().notifyRestartSaveFailure();
                return;
            }
#endif
        }
#endif

        Serial.println("[REBOOTER] ============================================");
        Serial.printf("[REBOOTER] Executing reboot: %s\n", _reason);
        Serial.println("[REBOOTER] ============================================");
        Serial.flush();

#if FASTBEE_ENABLE_STORAGE_CACHE
        // 重启前强制落盘延迟写入的脏配置，避免 debounce 窗口内的修改丢失
        if (ConfigStorage::isInitialized()) {
            ConfigStorage::getInstance().flushDirtyEntries(true);
        }
#endif

        // The expander stays powered across an MCU restart. Confirm configured
        // startup levels before resetting, without waiting for a busy I2C bus.
        if (!PeripheralManager::getInstance().shutdownIoExpanders()) {
            Serial.println("[REBOOTER] IO_SHUTDOWN_UNCONFIRMED: physical output state not confirmed");
        }

        // 保存重启前状态快照到 RTC 内存
        RestartDiagnostics::savePreRestartState(
            _restartReason,
            _reason);

        // 短暂延迟确保串口输出完成
        delay(100);

        ESP.restart();

        // ESP.restart() 不应返回，但如果返回了，保持 _scheduled=false 避免循环
    }
}

bool SystemRebooter::isScheduled() {
    return _scheduled;
}
