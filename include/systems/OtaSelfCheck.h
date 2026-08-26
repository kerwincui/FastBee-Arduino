/**
 * @file OtaSelfCheck.h
 * @brief OTA 升级后自检与回滚控制
 *
 * OTA 切到新分区后首次启动时，若 bootloader 启用了应用回滚
 * （CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE），运行分区处于
 * ESP_OTA_IMG_NEW 状态（待验证）：
 *   - 固件自检通过（稳定运行 N 秒）后调用 esp_ota_mark_app_valid_cancel_rollback()
 *     确认新固件有效，取消回滚；
 *   - 关键初始化失败时调用 esp_ota_mark_app_invalid_rollback() 立即回滚到
 *     上一稳定分区，避免"坏固件启动即崩溃→变砖"。
 * 若 bootloader 未启用回滚，分区状态不会进入 ESP_OTA_IMG_NEW，所有操作为无害空转。
 *
 * @date 2026-08-14
 */

#ifndef FASTBEE_OTA_SELF_CHECK_H
#define FASTBEE_OTA_SELF_CHECK_H

#include <Arduino.h>

class OtaSelfCheck {
public:
    /**
     * @brief 启动早期调用：检测运行分区是否处于 OTA 待确认状态
     * @note 在 Serial 就绪后、框架初始化前调用
     */
    static void begin();

    /**
     * @brief 主循环调用：自检窗口到期后确认新固件有效
     * @note 轻量级，非待确认状态时立即返回
     */
    static void handle();

    /**
     * @brief 关键初始化失败时调用：标记新固件无效并回滚
     * @param reason 失败原因（写入串口日志）
     * @note bootloader 未启用回滚时仅打印提示；不会返回到正常启动流程
     */
    static void markInvalidAndRollback(const char* reason);

    /**
     * @brief 是否处于 OTA 待确认状态
     */
    static bool isPending();

private:
    static bool _pending;          ///< 运行分区处于 PENDING_IMAGE_READY
    static bool _confirmed;        ///< 已确认过（防重复调用）
    static unsigned long _bootMs;  ///< begin() 时的 millis 基准
};

#endif  // FASTBEE_OTA_SELF_CHECK_H
