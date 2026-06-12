/**
 * @file RestartDiagnostics.cpp
 * @brief 设备异常重启诊断模块实现
 * 
 * 使用 RTC_NOINIT_ATTR 将重启前的系统状态快照保存到 RTC 慢速内存，
 * 该区域在软件重启（ESP.restart()）后不会被清零，但硬件上电重置会丢失。
 * 
 * 典型使用场景：
 * 1. 低内存触发自动重启前保存完整的 heap/栈/网络状态
 * 2. 下次启动时输出上次重启的详细诊断信息
 * 3. 帮助开发人员追踪间歇性重启问题
 */

#include "systems/RestartDiagnostics.h"
#include "utils/HeapFragmentation.h"
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <WiFi.h>

// ========== RTC_NOINIT 内存区域 ==========
// 这块内存在软件重启间保留数据，魔数校验确保数据有效性
static constexpr uint32_t SNAPSHOT_MAGIC = 0xFBD1A600;  // FastBee Diagnostics

RTC_NOINIT_ATTR static PreRestartSnapshot s_lastSnapshot;

// ========== 私有辅助方法 ==========

uint32_t RestartDiagnostics::calculateChecksum(const PreRestartSnapshot& snap) {
    // 简单 XOR 校验（排除 checksum 字段本身）
    const uint8_t* data = reinterpret_cast<const uint8_t*>(&snap);
    size_t len = offsetof(PreRestartSnapshot, checksum);
    uint32_t xorVal = 0;
    for (size_t i = 0; i < len; i++) {
        xorVal ^= (uint32_t)data[i] << ((i % 4) * 8);
    }
    return xorVal ^ SNAPSHOT_MAGIC;
}

void RestartDiagnostics::collectCurrentState(PreRestartSnapshot& snap) {
    memset(&snap, 0, sizeof(snap));
    snap.magic = SNAPSHOT_MAGIC;
    snap.timestamp = millis();
    
    // 内存状态
    snap.freeHeap = ESP.getFreeHeap();
    snap.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    snap.largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    snap.heapFragmentation = calculateHeapFragmentationPercent(
        snap.freeHeap,
        snap.largestFreeBlock);
    
    // 任务栈水位
    TaskHandle_t loopTask = xTaskGetHandle("loopTask");
    TaskHandle_t asyncTcp = xTaskGetHandle("async_tcp");
    TaskHandle_t mqttReconn = xTaskGetHandle("mqtt_reconn");
    
    if (loopTask)   snap.loopTaskWatermark = uxTaskGetStackHighWaterMark(loopTask) * 4;
    if (asyncTcp)   snap.asyncTcpWatermark = uxTaskGetStackHighWaterMark(asyncTcp) * 4;
    if (mqttReconn) snap.mqttReconnWatermark = uxTaskGetStackHighWaterMark(mqttReconn) * 4;
    
    // 网络状态
    snap.wifiConnected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    snap.wifiRssi = snap.wifiConnected ? static_cast<int8_t>(WiFi.RSSI()) : 0;
}

void RestartDiagnostics::printSnapshot(const PreRestartSnapshot& snap) {
    Serial.println("  ┌─── Pre-Restart State Snapshot ───────────────────────┐");
    Serial.printf( "  │ Uptime before restart: %lu ms (%lu s)                \n",
                   (unsigned long)snap.timestamp, (unsigned long)(snap.timestamp / 1000));
    Serial.println("  ├─── Memory ──────────────────────────────────────────┤");
    Serial.printf( "  │ Free heap:        %lu bytes                         \n", (unsigned long)snap.freeHeap);
    Serial.printf( "  │ Min free heap:    %lu bytes (all-time low)          \n", (unsigned long)snap.minFreeHeap);
    Serial.printf( "  │ Largest block:    %lu bytes                         \n", (unsigned long)snap.largestFreeBlock);
    Serial.printf( "  │ Fragmentation:    %d%%                              \n", (int)snap.heapFragmentation);
    
    static const char* guardLevelNames[] = { "NORMAL", "WARN", "SEVERE", "CRITICAL" };
    uint8_t lvl = snap.memGuardLevel < 4 ? snap.memGuardLevel : 0;
    Serial.printf( "  │ MemGuard level:   %s (%d)                           \n", guardLevelNames[lvl], (int)snap.memGuardLevel);
    Serial.printf( "  │ Low-mem counter:  %d consecutive                    \n", (int)snap.consecutiveLowMem);
    
    Serial.println("  ├─── Task Stacks ─────────────────────────────────────┤");
    Serial.printf( "  │ loopTask:    %u bytes remaining                     \n", (unsigned)snap.loopTaskWatermark);
    Serial.printf( "  │ async_tcp:   %u bytes remaining                     \n", (unsigned)snap.asyncTcpWatermark);
    Serial.printf( "  │ mqtt_reconn: %u bytes remaining                     \n", (unsigned)snap.mqttReconnWatermark);
    
    Serial.println("  ├─── System ──────────────────────────────────────────┤");
    Serial.printf( "  │ WiFi:            %s (RSSI: %d dBm)                  \n",
                   snap.wifiConnected ? "Connected" : "Disconnected", (int)snap.wifiRssi);
    Serial.printf( "  │ MQTT queue:      %d                                 \n", (int)snap.mqttQueueDepth);
    Serial.printf( "  │ SSE clients:     %d                                 \n", (int)snap.sseClientCount);
    Serial.printf( "  │ Active rules:    %d                                 \n", (int)snap.activeRuleCount);
    
    Serial.println("  ├─── Restart Info ────────────────────────────────────┤");
    Serial.printf( "  │ Reason code:     %d (%s)                            \n",
                   (int)snap.restartReason, getRestartReasonString(static_cast<RestartReason>(snap.restartReason)));
    if (snap.restartContext[0] != '\0') {
        Serial.printf("  │ Context:         %s                              \n", snap.restartContext);
    }
    Serial.println("  └─────────────────────────────────────────────────────┘");
}

// ========== 公开接口 ==========

const char* RestartDiagnostics::getResetReasonString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "Power-on reset";
        case ESP_RST_EXT:       return "External reset (pin)";
        case ESP_RST_SW:        return "Software restart (ESP.restart)";
        case ESP_RST_PANIC:     return "Exception/Panic (software fault)";
        case ESP_RST_INT_WDT:   return "Interrupt Watchdog (task deadlock)";
        case ESP_RST_TASK_WDT:  return "Task Watchdog (loop blocked >60s)";
        case ESP_RST_WDT:       return "Other Watchdog reset";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wakeup";
        case ESP_RST_BROWNOUT:  return "Brownout (voltage drop)";
        case ESP_RST_SDIO:      return "SDIO reset";
        default:                return "Unknown reset reason";
    }
}

const char* RestartDiagnostics::getRestartReasonString(RestartReason reason) {
    switch (reason) {
        case RestartReason::UNKNOWN:             return "Unknown/First boot";
        case RestartReason::CRITICAL_LOW_MEMORY: return "Critical low memory (HealthMonitor)";
        case RestartReason::FRAMEWORK_LOW_MEMORY:return "Low memory (Framework guard)";
        case RestartReason::USER_COMMAND:        return "User command (reboot)";
        case RestartReason::OTA_UPDATE:          return "OTA update completed";
        case RestartReason::UNCAUGHT_EXCEPTION:  return "Uncaught C++ exception";
        case RestartReason::WATCHDOG_TIMEOUT:    return "Watchdog timeout";
        case RestartReason::STACK_OVERFLOW:      return "Stack overflow detected";
        case RestartReason::MEMORY_COMPACTION:   return "Irrecoverable fragmentation";
        case RestartReason::PERIPHERAL_FAULT:    return "Peripheral hardware fault";
        case RestartReason::CONFIG_CORRUPTION:   return "Configuration file corrupted";
        default:                                 return "Unknown reason code";
    }
}

bool RestartDiagnostics::wasAbnormalRestart() {
    esp_reset_reason_t reason = esp_reset_reason();
    switch (reason) {
        case ESP_RST_PANIC:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
        case ESP_RST_BROWNOUT:
            return true;
        case ESP_RST_SW:
            // 软件重启可能是正常（OTA/用户命令）或异常（低内存保护）
            // 通过 RTC 快照进一步判断
            if (s_lastSnapshot.magic == SNAPSHOT_MAGIC) {
                uint32_t ck = calculateChecksum(s_lastSnapshot);
                if (ck == s_lastSnapshot.checksum) {
                    RestartReason r = static_cast<RestartReason>(s_lastSnapshot.restartReason);
                    return r == RestartReason::CRITICAL_LOW_MEMORY ||
                           r == RestartReason::FRAMEWORK_LOW_MEMORY ||
                           r == RestartReason::UNCAUGHT_EXCEPTION ||
                           r == RestartReason::STACK_OVERFLOW ||
                           r == RestartReason::MEMORY_COMPACTION ||
                           r == RestartReason::PERIPHERAL_FAULT;
                }
            }
            return false;
        default:
            return false;
    }
}

bool RestartDiagnostics::getLastSnapshot(PreRestartSnapshot& out) {
    if (s_lastSnapshot.magic != SNAPSHOT_MAGIC) return false;
    uint32_t ck = calculateChecksum(s_lastSnapshot);
    if (ck != s_lastSnapshot.checksum) return false;
    out = s_lastSnapshot;
    return true;
}

void RestartDiagnostics::savePreRestartState(RestartReason reason, const char* context) {
    collectCurrentState(s_lastSnapshot);
    s_lastSnapshot.restartReason = static_cast<uint8_t>(reason);
    
    if (context) {
        strncpy(s_lastSnapshot.restartContext, context, sizeof(s_lastSnapshot.restartContext) - 1);
        s_lastSnapshot.restartContext[sizeof(s_lastSnapshot.restartContext) - 1] = '\0';
    } else {
        s_lastSnapshot.restartContext[0] = '\0';
    }
    
    s_lastSnapshot.checksum = calculateChecksum(s_lastSnapshot);
    
    // 同步输出到串口（确保即使重启也能在串口看到）
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════╗");
    Serial.println("║  RESTART DIAGNOSTICS — Saving pre-restart state...      ║");
    Serial.println("╚══════════════════════════════════════════════════════════╝");
    Serial.printf("[RESTART] Reason: %s\n", getRestartReasonString(reason));
    if (context) Serial.printf("[RESTART] Context: %s\n", context);
    Serial.printf("[RESTART] Uptime: %lu ms | Heap: %lu | MinHeap: %lu | Largest: %lu | Frag: %d%%\n",
                  (unsigned long)s_lastSnapshot.timestamp,
                  (unsigned long)s_lastSnapshot.freeHeap,
                  (unsigned long)s_lastSnapshot.minFreeHeap,
                  (unsigned long)s_lastSnapshot.largestFreeBlock,
                  (int)s_lastSnapshot.heapFragmentation);
    Serial.printf("[RESTART] Stacks — loop:%u async_tcp:%u mqtt:%u\n",
                  (unsigned)s_lastSnapshot.loopTaskWatermark,
                  (unsigned)s_lastSnapshot.asyncTcpWatermark,
                  (unsigned)s_lastSnapshot.mqttReconnWatermark);
    Serial.flush();
}

void RestartDiagnostics::logBootDiagnostics() {
    esp_reset_reason_t espReason = esp_reset_reason();
    bool abnormal = wasAbnormalRestart();
    
    Serial.println();
    Serial.println("╔══════════════════════════════════════════════════════════╗");
    Serial.println("║          BOOT DIAGNOSTICS — Restart Analysis            ║");
    Serial.println("╠══════════════════════════════════════════════════════════╣");
    Serial.printf( "║  ESP Reset Reason: %s\n", getResetReasonString(espReason));
    Serial.printf( "║  Reset Code:       %d\n", (int)espReason);
    Serial.printf( "║  Abnormal Restart: %s\n", abnormal ? "*** YES ***" : "No");
    Serial.println("╠══════════════════════════════════════════════════════════╣");
    
    // 当前启动内存状态
    Serial.printf( "║  Current free heap:  %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf( "║  Current max alloc:  %lu bytes\n", (unsigned long)ESP.getMaxAllocHeap());
    Serial.println("╠══════════════════════════════════════════════════════════╣");
    
    // 检查 RTC 快照有效性
    PreRestartSnapshot snap;
    if (getLastSnapshot(snap)) {
        Serial.println("║  Previous restart state snapshot FOUND:");
        printSnapshot(snap);
        
        // 异常重启时输出额外的高亮警告
        if (abnormal) {
            Serial.println();
            Serial.println("  *** WARNING: Device experienced abnormal restart! ***");
            Serial.printf( "  *** Previous uptime: %lu s | Heap at restart: %lu bytes ***\n",
                           (unsigned long)(snap.timestamp / 1000),
                           (unsigned long)snap.freeHeap);
            
            // 标记关键风险
            if (snap.freeHeap < 8192) {
                Serial.println("  >>> ROOT CAUSE LIKELY: Critical memory exhaustion <<<");
            }
            if (snap.loopTaskWatermark > 0 && snap.loopTaskWatermark < 512) {
                Serial.println("  >>> ROOT CAUSE LIKELY: loopTask stack overflow <<<");
            }
            if (snap.heapFragmentation > 80) {
                Serial.println("  >>> CONTRIBUTING FACTOR: Severe heap fragmentation <<<");
            }
        }
    } else {
        if (espReason == ESP_RST_POWERON) {
            Serial.println("║  No previous snapshot (first power-on, expected)");
        } else if (espReason == ESP_RST_PANIC || espReason == ESP_RST_INT_WDT ||
                   espReason == ESP_RST_TASK_WDT || espReason == ESP_RST_WDT) {
            Serial.println("║  *** No snapshot available — crash was too sudden ***");
            Serial.println("║  (Hard fault/WDT bypassed graceful state save)");
        } else {
            Serial.println("║  No previous snapshot (RTC data invalid or cleared)");
        }
    }
    
    Serial.println("╚══════════════════════════════════════════════════════════╝");
    Serial.println();
    
    // 对于 WDT/panic 类硬重启，输出额外诊断建议
    if (espReason == ESP_RST_TASK_WDT) {
        Serial.println("[DIAG] Task Watchdog triggered — a task was blocked for >60s.");
        Serial.println("[DIAG] Check: 1) Infinite loop  2) Deadlock  3) Blocking I/O");
    } else if (espReason == ESP_RST_INT_WDT) {
        Serial.println("[DIAG] Interrupt Watchdog — ISR or critical section too long.");
        Serial.println("[DIAG] Check: 1) ISR duration  2) portENTER_CRITICAL time");
    } else if (espReason == ESP_RST_PANIC) {
        Serial.println("[DIAG] Software panic — unrecoverable exception occurred.");
        Serial.println("[DIAG] Check: 1) Stack overflow  2) Null pointer  3) Alignment fault");
    } else if (espReason == ESP_RST_BROWNOUT) {
        Serial.println("[DIAG] Brownout detected — supply voltage dropped below threshold.");
        Serial.println("[DIAG] Check: 1) Power supply  2) USB cable  3) Current draw spikes");
    }
}
