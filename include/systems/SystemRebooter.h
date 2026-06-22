/**
 * @file SystemRebooter.h
 * @brief 统一的延迟重启工具类
 *
 * 用于 Web Handler 保存配置后安全地延迟重启设备。
 * 确保 HTTP 响应先发送完成，再执行 ESP.restart()。
 *
 * 典型用法：
 *   1. Web handler 保存配置到文件
 *   2. 发送 JSON 响应（含 restartRequired: true）
 *   3. 调用 SystemRebooter::scheduleConfigReboot("Network config changed")
 *   4. 主循环中 SystemRebooter::update() 在延迟后执行重启
 */
#pragma once

#include <Arduino.h>
#include "systems/RestartDiagnostics.h"

class SystemRebooter {
public:
    /**
     * @brief 调度一次延迟重启
     * @param reason  重启原因描述（最多 47 字符，会保存到 RTC 诊断）
     * @param delayMs 延迟毫秒数（默认 2000ms，确保 HTTP 响应已发送）
     * @param restartReason 重启原因枚举（默认 CONFIG_CHANGE）
     */
    static void scheduleReboot(const char* reason,
                               unsigned long delayMs = 2000,
                               RestartReason restartReason = RestartReason::CONFIG_CHANGE);

    /**
     * @brief 调度配置变更重启（便捷方法，reason=CONFIG_CHANGE）
     */
    static void scheduleConfigReboot(const char* reason, unsigned long delayMs = 2000);

    /**
     * @brief 取消已调度的重启（例如后续操作失败需要回滚时）
     */
    static void cancelScheduledReboot();

    /**
     * @brief 在 loop/update 中调用，到达延迟时间后执行重启
     *
     * 执行流程：
     *   1. RestartDiagnostics::savePreRestartState(CONFIG_CHANGE, reason)
     *   2. Serial.flush()
     *   3. ESP.restart()
     */
    static void update();

    /**
     * @brief 检查是否有待执行的重启
     */
    static bool isScheduled();

private:
    static bool _scheduled;
    static unsigned long _rebootAt;
    static char _reason[48];
    static RestartReason _restartReason;
};
