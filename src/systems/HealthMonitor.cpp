/**
 * @description: 系统健康监控实现
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:32:41
 *
 * 优化说明：
 *  1. getHealthReport() 改为写 char 缓冲区，减少 String 拼接开销
 *  2. 新增 shutdown() 方法，完善资源释放逻辑
 *  3. 文件系统检测改为检测挂载状态而非特定文件
 *  4. 常量引用 SystemConstants 中已定义的阈值
 */

#include "systems/HealthMonitor.h"
#include "systems/LoggerSystem.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <core/SystemConstants.h>
#include <core/FeatureFlags.h>
#include "core/FastBeeFramework.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecScheduler.h"
#include "network/handlers/HandlerUtils.h"
#if FASTBEE_ENABLE_MQTT
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"
#include "network/WebConfigManager.h"
#include "network/handlers/SSERouteHandler.h"
#endif
#if FASTBEE_ENABLE_MODBUS
#include "protocols/ProtocolManager.h"
#include "protocols/ModbusHandler.h"
#endif
#if FASTBEE_ENABLE_STORAGE_CACHE
#include "systems/ConfigStorage.h"
#endif

HealthMonitor::HealthMonitor()
    : lastCheckTime(0),
      lastStackLogTime(0),
      lastMetricsLogTime(0),
      heapWatermark(UINT32_MAX),
      consecutiveLowMemCount(0),
      consecutiveFragmentationCriticalCount(0),
      running(false),
      _compactionTriggered(false),
      mqttQueueDepth(0),
      sseClientCount(0),
      pollDurationMs(0) {
    memset(&currentHealth, 0, sizeof(currentHealth));
}

bool HealthMonitor::initialize() {
    running = true;
    performHealthCheck();
    LOG_INFO("Health Monitor: Initialized");
    return true;
}

void HealthMonitor::shutdown() {
    running = false;
    LOG_INFO("Health Monitor: Shutdown");
}

void HealthMonitor::update() {
    if (!running) return;

    unsigned long currentTime = millis();
    // 健康检查间隔：5 秒
    if (currentTime - lastCheckTime >= 5000UL) {
        performHealthCheck();
        updateMemoryGuardLevel();
        checkCriticalMemory();
        lastCheckTime = currentTime;
    }
    // 任务栈水位日志：每 60 秒输出一次
    if (currentTime - lastStackLogTime >= 60000UL) {
        logTaskStackWatermarks();
        lastStackLogTime = currentTime;
    }
    // 关键指标摘要：每 60 秒串口输出一次
    if (currentTime - lastMetricsLogTime >= 60000UL) {
        logMetricsSummary();
        lastMetricsLogTime = currentTime;
    }
}

void HealthMonitor::performHealthCheck() {
    // ── 内存状态 ─────────────────────────────────────────────────────────
    // ── DRAM 内部内存（排除 PSRAM，用于 WiFi/MQTT/SSL 保护决策）─────────
    currentHealth.dramFreeHeap     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    currentHealth.dramLargestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    // ── 内存状态（含 PSRAM 的全局视图）───────────────────────
    currentHealth.freeHeap    = esp_get_free_heap_size();
    currentHealth.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    if (currentHealth.freeHeap < heapWatermark) {
        heapWatermark = currentHealth.freeHeap;
    }

    // DRAM 水位趋势采样（泄漏检测）
    if (currentHealth.dramFreeHeap < _dramWatermarkSinceLastSample) {
        _dramWatermarkSinceLastSample = currentHealth.dramFreeHeap;
    }
    unsigned long nowMs = millis();
    if (_lastWatermarkSampleMs == 0) {
        _lastWatermarkSampleMs = nowMs;  // 首次初始化
    } else if (nowMs - _lastWatermarkSampleMs >= WATERMARK_SAMPLE_INTERVAL_MS) {
        // 记录过去 1 小时的 DRAM 最低水位
        _dramWatermarkHistory[_dramHistoryIndex] = _dramWatermarkSinceLastSample;
        _dramHistoryIndex = (_dramHistoryIndex + 1) % WATERMARK_HISTORY_SIZE;
        if (_dramHistoryCount < WATERMARK_HISTORY_SIZE) _dramHistoryCount++;
        _dramWatermarkSinceLastSample = UINT32_MAX;
        _lastWatermarkSampleMs = nowMs;

        // 每次采样后检查泄漏趋势
        if (_dramHistoryCount >= 6 && detectMemoryLeak()) {
            LOG_WARNING("[HealthMonitor] Possible memory leak detected: DRAM watermark declining trend");
        }
    }

    // 碎片率 = 1 - (最大连续块 / 总空闲)，基于 DRAM 内部数据（排除 PSRAM 干扰）
    // DRAM 的碎片率才是影响 WiFi/TCP/SSL 分配的真实指标
    currentHealth.largestFreeBlock = currentHealth.dramLargestBlock;
    currentHealth.heapFragmentation = calculateHeapFragmentationPercent(
        currentHealth.dramFreeHeap,
        currentHealth.dramLargestBlock);

    // ── 文件系统状态（检测挂载而非特定文件）──────────────────────────────
    // LittleFS 挂载后 totalBytes() > 0 即为正常
    currentHealth.fileSystemOK = (LittleFS.totalBytes() > 0);

    // ── 网络状态 ─────────────────────────────────────────────────────────
    currentHealth.wifiConnected = (WiFi.status() == WL_CONNECTED);
    currentHealth.wifiStrength  = currentHealth.wifiConnected
        ? static_cast<int8_t>(WiFi.RSSI())
        : 0;

    // ── 系统时间 ─────────────────────────────────────────────────────────
    currentHealth.uptime = millis();

    // CPU 占用估算（暂简化为 0，实际需基于空闲任务计数器）
    currentHealth.cpuUsage = 0;
}

// 写入 char 缓冲区，零 String 对象
size_t HealthMonitor::getHealthReport(char* buf, size_t bufSize) const {
    return snprintf(buf, bufSize,
        "Health: heap=%luB frag=%d%% cpu=%d%% fs=%s wifi=%s rssi=%ddBm up=%lus",
        (unsigned long)currentHealth.freeHeap,
        (int)currentHealth.heapFragmentation,
        (int)currentHealth.cpuUsage,
        currentHealth.fileSystemOK  ? "OK"  : "ERR",
        currentHealth.wifiConnected ? "UP"  : "DOWN",
        (int)currentHealth.wifiStrength,
        (unsigned long)(currentHealth.uptime / 1000UL));
}

// String 兼容接口
String HealthMonitor::getHealthReport() {
    char buf[160];
    getHealthReport(buf, sizeof(buf));
    return String(buf);
}

bool HealthMonitor::isSystemHealthy() const {
    return currentHealth.freeHeap         >= HealthCheck::MIN_FREE_HEAP
        && currentHealth.heapFragmentation <= HealthCheck::MAX_HEAP_FRAGMENTATION
        && currentHealth.fileSystemOK;
}

// 低内存保护：分级恢复机制（替代原有的 15s 硬重启）
// EMERGENCY: DRAM < 4KB → 立即禁用 MQTT + 停止所有非 Web 服务
// CRITICAL 持续 30s → 写入配置禁用 MQTT（重启后也不连）
// CRITICAL 持续 90s → 安全重启（boot loop 保护：启动 < 120s 不重启）
void HealthMonitor::checkCriticalMemory() {
    // 使用 DRAM 内部内存进行检测（排除 PSRAM）
    // WiFi/MQTT/SSL 都不能用 PSRAM，使用 DRAM 空闲来评估真实风险
    const uint32_t checkHeap = currentHealth.dramFreeHeap;

    // 根据 PSRAM 容量动态调整阈值
    // F8R0 (无 PSRAM): DRAM 总量 ~200KB，8KB 临界合理
    // F8R4 (4MB PSRAM): DRAM 总量 ~320KB，10KB 临界更宽松
    // F16R8 (8MB PSRAM): DRAM 总量 ~320KB，12KB 临界更宽松
    static uint32_t criticalThreshold = 0;
    static uint32_t warningThreshold = 0;
    if (criticalThreshold == 0) {
        // 仅在首次调用时计算一次（PSRAM 大小在运行时不会变化）
        size_t psramSize = ESP.getPsramSize();
        if (psramSize >= 8 * 1024 * 1024) {
            // 8MB PSRAM (F16R8): DRAM 压力较轻，阈值更宽松
            criticalThreshold = 12288;  // 12KB
            warningThreshold  = 24576;  // 24KB
        } else if (psramSize >= 4 * 1024 * 1024) {
            // 4MB PSRAM (F8R4): 中等阈值
            criticalThreshold = 10240;  // 10KB
            warningThreshold  = 20480;  // 20KB
        } else {
            // 无 PSRAM (F8R0): 保守阈值
            criticalThreshold = 8192;   // 8KB
            warningThreshold  = 16384;  // 16KB
        }
    }

    // ── 紧急兜底：DRAM < 4KB，系统随时可能 crash，不等计时立即执行所有恢复 ──
    static constexpr uint32_t EMERGENCY_DRAM_THRESHOLD = 4096;
    if (checkHeap < EMERGENCY_DRAM_THRESHOLD && !_mqttDisabledForMemory) {
        Serial.printf("[MEMRECOVER] EMERGENCY: DRAM=%lu < 4KB, immediate full recovery\n",
                      (unsigned long)checkHeap);
        // 立即执行 SEVERE 恢复（如果还没做）
        performMemoryRecovery(MemoryGuardLevel::NORMAL, MemoryGuardLevel::CRITICAL);
        // 立即禁用 MQTT
        _mqttDisabledForMemory = true;
        _mqttStoppedForMemory = true;
        disableMqttForMemory();
        Serial.printf("[MEMRECOVER] EMERGENCY complete: DRAM=%lu, Web stays available\n",
                      (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    if (checkHeap < criticalThreshold) {
        consecutiveLowMemCount++;

        // 开始 CRITICAL 计时（首次进入）
        if (_criticalStartTime == 0) {
            _criticalStartTime = millis();
            Serial.printf("[MEMRECOVER] CRITICAL DRAM detected: dram=%lu threshold=%lu, starting recovery timer\n",
                          (unsigned long)checkHeap, (unsigned long)criticalThreshold);
        }
        unsigned long criticalDuration = millis() - _criticalStartTime;

        char buf[128];
        snprintf(buf, sizeof(buf),
                 "Health: CRITICAL low DRAM! dram=%lu maxBlock=%lu count=%lu duration=%lus",
                 (unsigned long)checkHeap,
                 (unsigned long)currentHealth.dramLargestBlock,
                 (unsigned long)consecutiveLowMemCount,
                 (unsigned long)(criticalDuration / 1000));
        LOG_ERROR(buf);

        // 确保 SEVERE 恢复措施已执行（兜底：等级可能跳过 SEVERE 直达 CRITICAL）
        if (!_mqttsMemoryDowngrade && !_modbusStoppedForMemory && !_periphExecPausedForMemory) {
            Serial.println("[MEMRECOVER] SEVERE fallback: executing recovery from CRITICAL path");
            performMemoryRecovery(MemoryGuardLevel::WARN, MemoryGuardLevel::CRITICAL);
        }

#if FASTBEE_ENABLE_MQTT
        // 30s: 写入配置禁用 MQTT（重启后也不连）+ 立即停止
        if (criticalDuration >= CRITICAL_MQTT_STOP_DELAY_MS && !_mqttDisabledForMemory) {
            _mqttDisabledForMemory = true;
            _mqttStoppedForMemory = true;
            Serial.printf("[MEMRECOVER] === CRITICAL %lus: disabling MQTT permanently ===\n",
                          (unsigned long)(criticalDuration / 1000));
            disableMqttForMemory();
            Serial.println("[MEMRECOVER]   User must re-enable MQTT via Web UI after memory recovers");
        }
#endif

        // 90s: 安全重启（boot loop 保护 + 恢复轨迹总结）
        if (criticalDuration >= CRITICAL_REBOOT_DELAY_MS && !SystemRebooter::isScheduled()) {
            // 重启循环保护：启动 < 120s 不重启，防止 boot loop
            if (millis() < 120000UL) {
                static unsigned long lastBootLoopWarn = 0;
                if (millis() - lastBootLoopWarn > 30000UL) {
                    Serial.println("[MEMRECOVER] Skip reboot: recent boot (<120s), possible boot loop. Web stays available.");
                    lastBootLoopWarn = millis();
                }
                return;  // 不重启，保持 Web 可用
            }

            // 输出恢复轨迹总结
            Serial.println("[MEMRECOVER] === Recovery summary before reboot ===");
            Serial.printf("[MEMRECOVER]   Uptime: %lus, CRITICAL duration: %lus\n",
                          (unsigned long)(millis() / 1000),
                          (unsigned long)(criticalDuration / 1000));
            Serial.printf("[MEMRECOVER]   Actions taken: %s%s%s%s\n",
                          _mqttsMemoryDowngrade ? "MQTTS->MQTT(SEVERE) " : "",
                          _modbusStoppedForMemory ? "Modbus stopped " : "",
                          _periphExecPausedForMemory ? "PeriphExec paused " : "",
                          _mqttDisabledForMemory ? "MQTT disabled(30s)" : "");
            Serial.printf("[MEMRECOVER]   DRAM: current=%lu largest=%lu\n",
                          (unsigned long)checkHeap,
                          (unsigned long)currentHealth.dramLargestBlock);
            Serial.printf("[MEMRECOVER]   Reboot reason: DRAM<%luKB for %lus, all recovery exhausted\n",
                          (unsigned long)(criticalThreshold / 1024),
                          (unsigned long)(criticalDuration / 1000));

            char reasonBuf[48];
            snprintf(reasonBuf, sizeof(reasonBuf), "DRAM<%luKB for %lus",
                     (unsigned long)(criticalThreshold / 1024),
                     (unsigned long)(criticalDuration / 1000));
            SystemRebooter::scheduleReboot(reasonBuf, 3000, RestartReason::CRITICAL_LOW_MEMORY);
        }
    } else if (checkHeap < warningThreshold) {
        consecutiveLowMemCount = 0;
        // _criticalStartTime 由 applyDegradation() NORMAL 路径和 restoreMemoryRecovery() 统一管理
        char buf[96];
        snprintf(buf, sizeof(buf), "Health: Low DRAM warning dram=%lu total=%lu maxBlock=%lu",
                 (unsigned long)checkHeap,
                 (unsigned long)currentHealth.freeHeap,
                 (unsigned long)currentHealth.dramLargestBlock);
        LOG_WARNING(buf);
    } else {
        consecutiveLowMemCount = 0;
        // _criticalStartTime 由 applyDegradation() NORMAL 路径和 restoreMemoryRecovery() 统一管理
    }
}

// 定期输出关键任务的栈水位
void HealthMonitor::logTaskStackWatermarks() {
    // 获取当前任务的栈水位
    TaskHandle_t tasks[] = { NULL, NULL, NULL };
    const char* names[] = { "loopTask", "async_tcp", "mqtt_reconn" };
    
    for (int i = 0; i < 3; i++) {
        tasks[i] = xTaskGetHandle(names[i]);
        if (tasks[i]) {
            UBaseType_t watermark = uxTaskGetStackHighWaterMark(tasks[i]);
            // 水位低于 512 字节时警告（栈溢出风险）
            if (watermark < 128) {  // FreeRTOS 返回的是 word 数（ESP32 上 1 word = 4 bytes）
                char buf[96];
                snprintf(buf, sizeof(buf), "Health: Task '%s' stack watermark CRITICALLY LOW: %u words (%u bytes)",
                         names[i], (unsigned)watermark, (unsigned)(watermark * 4));
                LOG_ERROR(buf);
            } else {
                char buf[80];
                snprintf(buf, sizeof(buf), "Health: Task '%s' stack watermark: %u words (%u bytes)",
                         names[i], (unsigned)watermark, (unsigned)(watermark * 4));
                LOG_DEBUG(buf);
            }
        }
    }
}

// ── MemGuard 等级判断 ────────────────────────────────────────────────

void HealthMonitor::updateMemoryGuardLevel() {
    uint32_t freeHeap = currentHealth.dramFreeHeap;
    // 使用 DRAM 内部连续块做等级判断（排除 PSRAM 干扰）
    // MALLOC_CAP_DEFAULT 在有 PSRAM 的设备上会包含 PSRAM 的巨大连续块（如 8MB）
    // 导致 CRITICAL 判断永远不会触发，实际 DRAM 被消耗却检测不到
    uint32_t largestBlock = currentHealth.dramLargestBlock;  // DRAM 内部连续块
    MemoryGuardLevel newLevel;

    // 启动期 grace period：前 90 秒为框架 + lwip + AsyncTCP + NTP + 日志 flush 等
    // 并发初始化阶段，ESP32 no-PSRAM 堆会出现一次性高碎片/小 largestBlock 峰值。
    // 此期间不基于 largestBlock / 碎片率 主动提升到 SEVERE/CRITICAL（仅告警），
    // 避免在系统尚未稳定时误触发永久降级导致主循环卡死。
    // freeHeap 绝对值低的强降级仍然保留——那是真正的危险信号。
    const bool inStartupGrace = (millis() < 90000UL);

    // 基于 freeHeap 的初步判断
    if (freeHeap >= MEM_THRESHOLD_NORMAL) {
        newLevel = MemoryGuardLevel::NORMAL;
    } else if (freeHeap >= MEM_THRESHOLD_WARN) {
        newLevel = MemoryGuardLevel::WARN;
    } else if (freeHeap >= MEM_THRESHOLD_SEVERE) {
        newLevel = MemoryGuardLevel::SEVERE;
    } else {
        newLevel = MemoryGuardLevel::CRITICAL;
    }

    // 健康反证：freeHeap 虽略低，但 largestBlock 足够大时保持 NORMAL
    // （ESP32 常态 freeHeap ~27KB、largest ~22KB 是健康运行区间，不应降级）
    if (newLevel == MemoryGuardLevel::WARN && largestBlock >= MEM_LARGEST_HEALTHY) {
        newLevel = MemoryGuardLevel::NORMAL;
    }

    // 基于 largestFreeBlock 的强制提升：即使 freeHeap 看起来正常，
    // 如果最大可分配块过小，系统无法工作。
    // 注意：MAX_CONNECTIONS=4 + Connection:close 时正常 web 服务期间 largest 会临时降到 3-8KB，
    // 因此 CRITICAL 阈值设为 8KB，SEVERE 设为 12KB，避免访问网页时误触发降级。
    if (largestBlock < FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK) {
        // 最大块 < 8KB：内存碎片严重，系统分配能力受限
        if (inStartupGrace) {
            static uint32_t lastGraceWarnCrit = 0;
            uint32_t now = millis();
            if (now - lastGraceWarnCrit > 10000UL) {
                lastGraceWarnCrit = now;
                Serial.printf("[MEMGUARD] startup-grace(%lus): largestBlock=%lu<%uB (skip CRITICAL)\n",
                              (unsigned long)(now / 1000), (unsigned long)largestBlock,
                              (unsigned)FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK);
            }
        } else if (newLevel != MemoryGuardLevel::CRITICAL) {
            Serial.printf("[MEMGUARD] largestFreeBlock=%lu < %uB, elevating to CRITICAL\n",
                          (unsigned long)largestBlock,
                          (unsigned)FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK);
            newLevel = MemoryGuardLevel::CRITICAL;
        }
    } else if (largestBlock < FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK) {
        // 最大块 < 12KB：内存压力较大
        if (inStartupGrace) {
            static uint32_t lastGraceWarnSev = 0;
            uint32_t now = millis();
            if (now - lastGraceWarnSev > 10000UL) {
                lastGraceWarnSev = now;
                Serial.printf("[MEMGUARD] startup-grace(%lus): largestBlock=%lu<%uB (skip SEVERE)\n",
                              (unsigned long)(now / 1000), (unsigned long)largestBlock,
                              (unsigned)FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK);
            }
        } else if (newLevel < MemoryGuardLevel::SEVERE) {
            Serial.printf("[MEMGUARD] largestFreeBlock=%lu < %uB, elevating to SEVERE\n",
                          (unsigned long)largestBlock,
                          (unsigned)FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK);
            newLevel = MemoryGuardLevel::SEVERE;
        }
    }

    // 碎片率过高时至少提升到 WARN，若 largestFreeBlock 也小则进一步提升
    // 启动期同样只告警不升级（碎片率在启动高峰后通常会自然回落）
    if (isFragmentationHigh()) {
        if (inStartupGrace) {
            static uint32_t lastGraceWarnFrag = 0;
            uint32_t now = millis();
            if (now - lastGraceWarnFrag > 10000UL) {
                lastGraceWarnFrag = now;
                Serial.printf("[MEMGUARD] startup-grace(%lus): frag=%d%% largest=%lu (skip elevation)\n",
                              (unsigned long)(now / 1000),
                              (int)currentHealth.heapFragmentation,
                              (unsigned long)largestBlock);
            }
        } else if (newLevel == MemoryGuardLevel::NORMAL) {
            newLevel = MemoryGuardLevel::WARN;
            Serial.printf("[MEMGUARD] High fragmentation (%d%%), elevating to WARN\n",
                          (int)currentHealth.heapFragmentation);
        } else if (newLevel == MemoryGuardLevel::WARN &&
                   largestBlock < FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK) {
            newLevel = MemoryGuardLevel::SEVERE;
            Serial.printf("[MEMGUARD] High fragmentation (%d%%) + small block=%lu, elevating to SEVERE\n",
                          (int)currentHealth.heapFragmentation, (unsigned long)largestBlock);
        }
    }

    // 等级变化时执行降级/恢复措施
    if (newLevel != _currentLevel) {
        Serial.printf("[MEMGUARD] Level changed: %d -> %d (heap=%u frag=%d%%)\n",
                      (int)_currentLevel, (int)newLevel, freeHeap,
                      (int)currentHealth.heapFragmentation);
        MemoryGuardLevel oldLevel = _currentLevel;
        _currentLevel = newLevel;
        applyDegradation(oldLevel, newLevel);
    }

    // 碎片率检查：超过阈值时触发紧凑化策略
    if (currentHealth.heapFragmentation > FRAG_THRESHOLD_COMPACT) {
        if (!_compactionTriggered) {
            _compactionTriggered = true;
            compactMemory();
        }
    } else {
        _compactionTriggered = false;  // 碎片率恢复正常，重置标志
    }

    const bool severeFragmentation =
        !inStartupGrace &&
        currentHealth.heapFragmentation >= FRAG_THRESHOLD_REBOOT &&
        currentHealth.dramLargestBlock > 0 &&
        currentHealth.dramLargestBlock < FRAG_REBOOT_MAX_BLOCK;

    if (severeFragmentation) {
        consecutiveFragmentationCriticalCount++;
        if (consecutiveFragmentationCriticalCount == 1 ||
            consecutiveFragmentationCriticalCount == FRAG_REBOOT_COUNT) {
            LOG_WARNINGF("[MEMGUARD] Severe fragmentation persists: frag=%u%% dramLargest=%lu count=%lu/%lu",
                         (unsigned)currentHealth.heapFragmentation,
                         (unsigned long)currentHealth.dramLargestBlock,
                         (unsigned long)consecutiveFragmentationCriticalCount,
                         (unsigned long)FRAG_REBOOT_COUNT);
        }

        if (consecutiveFragmentationCriticalCount >= FRAG_REBOOT_COUNT &&
            !SystemRebooter::isScheduled()) {
            char reason[48];
            snprintf(reason, sizeof(reason), "DRAM frag=%u%% block=%lu",
                     (unsigned)currentHealth.heapFragmentation,
                     (unsigned long)currentHealth.dramLargestBlock);
            LOG_ERROR("[MEMGUARD] Fragmentation unrecoverable, scheduling reboot");
            SystemRebooter::scheduleReboot(reason, 2000UL, RestartReason::MEMORY_COMPACTION);
        }
    } else {
        consecutiveFragmentationCriticalCount = 0;
    }
}

// ── MemGuard 降级/恢复措施 ─────────────────────────────────────────
void HealthMonitor::applyDegradation(MemoryGuardLevel oldLevel, MemoryGuardLevel newLevel) {
    auto& logger = LoggerSystem::getInstance();

    // ── 恢复到 NORMAL：还原所有设置 ─────────────────────────────────────
    if (newLevel == MemoryGuardLevel::NORMAL) {
        if (_degradationActive) {
            logger.setLogLevel(_savedLogLevel);
            logger.enableFileLogging(_savedFileLogging);
            _degradationActive = false;
            Serial.println("[MEMGUARD] Restored: logLevel & fileLogging");
            // 恢复 MQTT 上报频率
#if FASTBEE_ENABLE_MQTT
            {
                FastBeeFramework* fw = FastBeeFramework::getInstance();
                ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
                MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
                if (mqtt) {
                    mqtt->setMinReportInterval(0);
                    Serial.println("[MEMGUARD] MQTT report interval restored");
                }
            }
#endif
            // 恢复因内存压力停止的服务（Modbus、PeriphExec、MQTT）
            restoreMemoryRecovery();
        }
        _criticalStartTime = 0;  // 重置 CRITICAL 计时
        return;
    }

    // ── 首次进入降级：保存当前日志设置 ──────────────────────────────────
    if (!_degradationActive) {
        _savedLogLevel = logger.getLogLevel();
        _savedFileLogging = logger.isFileLoggingEnabled();
        _degradationActive = true;
    }

    // ── WARN 级及以上：降低日志级别到 WARNING ──────────────────────────
    if (newLevel >= MemoryGuardLevel::WARN) {
        if (logger.getLogLevel() < LOG_WARNING) {  // 仅在当前级别更低（更详细）时调整
            logger.setLogLevel(LOG_WARNING);
            Serial.println("[MEMGUARD] WARN: log level -> WARNING");
        }
    }

    // ── SEVERE 级及以上：禁用文件日志 + MQTT 降采样 ────────────────────
    if (newLevel >= MemoryGuardLevel::SEVERE) {
        if (logger.isFileLoggingEnabled()) {
            logger.enableFileLogging(false);
            Serial.println("[MEMGUARD] SEVERE: file logging disabled");
        }
        // 强制关闭所有 SSE 客户端：释放 lwip TCP 缓冲区，避免 Web 请求无法应答
        // 仅在从低级别升级到 SEVERE/CRITICAL 的跳变时触发一次
        if (oldLevel < MemoryGuardLevel::SEVERE) {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            WebConfigManager* wcm = fw ? fw->getWebConfigManager() : nullptr;
            SSERouteHandler* sse = wcm ? wcm->getSseRouteHandler() : nullptr;
            if (sse) {
                size_t closed = sse->closeAllClients();
                Serial.printf("[MEMGUARD] SEVERE: closed %u SSE clients to reclaim memory\n",
                              (unsigned)closed);
            }
        }
        // MQTT 降采样：降低上报频率以减少内存压力
#if FASTBEE_ENABLE_MQTT
        {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
            MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
            if (mqtt) {
                uint32_t interval = (newLevel == MemoryGuardLevel::CRITICAL) ? 30000 : 10000;
                mqtt->setMinReportInterval(interval);
                Serial.printf("[MEMGUARD] MQTT report interval -> %lu ms\n", (unsigned long)interval);
            }
        }
#endif
        // 内存恢复：MQTTS→MQTT 降级 + 停止 Modbus + 暂停 PeriphExec
        // 仅在首次进入 SEVERE 或从低级别跳入时执行（避免重复操作）
        if (oldLevel < MemoryGuardLevel::SEVERE) {
            performMemoryRecovery(oldLevel, newLevel);
        }
    }

    // ── CRITICAL 级：提升日志级别到 ERROR，确保文件日志已禁用 ──────────
    if (newLevel == MemoryGuardLevel::CRITICAL) {
        if (logger.getLogLevel() < LOG_ERROR) {
            logger.setLogLevel(LOG_ERROR);
            Serial.println("[MEMGUARD] CRITICAL: log level -> ERROR");
        }
    }
}

void HealthMonitor::compactMemory() {
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "[MEMGUARD] High fragmentation (%d%%), performing targeted reclamation",
             (int)currentHealth.heapFragmentation);
    LOG_WARNING(logBuf);

    const size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t dramBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // 策略1: 刷新日志缓冲区（释放待写入文件的日志数据占用的堆内存）
    LoggerSystem::getInstance().flushBuffer();

    // 策略2: 关闭残留 SSE 客户端（lwIP TCP 缓冲区是碎片化最大来源）
    // MemGuard SEVERE 级别已关闭 SSE，但碎片化可能在 WARN 级别就触发，
    // 此时仍可能有 SSE 长连接持有大块 TCP 缓冲区
    {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        WebConfigManager* wcm = fw ? fw->getWebConfigManager() : nullptr;
        SSERouteHandler* sse = wcm ? wcm->getSseRouteHandler() : nullptr;
        if (sse && sse->clientCount() > 0) {
            size_t closed = sse->closeAllClients();
            Serial.printf("[MEMGUARD] Closed %u SSE clients for defragmentation\n",
                          (unsigned)closed);
            // 给 lwIP 短暂时间释放 TCP PCB
            delay(50);
        }
    }

    // 策略3: 清除 ConfigStorage 缓存（如果启用）
#if FASTBEE_ENABLE_STORAGE_CACHE
    ConfigStorage::getInstance().clearCache();
#endif

    const size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const uint32_t dramAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[MEMGUARD] Compaction result: dram=%lu->%lu largest=%lu->%lu (delta=%+d)\n",
                  (unsigned long)dramBefore,
                  (unsigned long)dramAfter,
                  (unsigned long)largestBefore,
                  (unsigned long)largestAfter,
                  (int)(largestAfter - largestBefore));

    // 记录碎片化事件用于重启后诊断
    if (largestAfter <= largestBefore) {
        Serial.println("[MEMGUARD] Compaction did not improve largest block — relying on reboot watchdog");
    }
}

MemoryGuardLevel HealthMonitor::getMemoryGuardLevel() const {
    return _currentLevel;
}

bool HealthMonitor::isMemoryNormal() const {
    return _currentLevel == MemoryGuardLevel::NORMAL;
}

bool HealthMonitor::isMemoryWarning() const {
    return _currentLevel == MemoryGuardLevel::WARN;
}

bool HealthMonitor::isMemorySevere() const {
    return _currentLevel == MemoryGuardLevel::SEVERE;
}

bool HealthMonitor::isMemoryCritical() const {
    if (_currentLevel == MemoryGuardLevel::CRITICAL) return true;
    // 即使等级尚未更新，直接检查 largestFreeBlock 作为兜底
    return currentHealth.largestFreeBlock < 4096;
}

bool HealthMonitor::isFragmentationHigh() const {
    // 统一使用 DRAM 指标计算碎片率，与 performHealthCheck() 保持一致
    // 避免 PSRAM 设备因 PSRAM 大块空闲而掩盖 DRAM 碎片问题
    uint8_t frag = calculateHeapFragmentationPercent(
        currentHealth.dramFreeHeap,
        currentHealth.dramLargestBlock);
    return frag > FRAG_THRESHOLD_COMPACT;
}

// ── 外部模块上报指标 setter ──────────────────────────────────────────
void HealthMonitor::setMqttQueueDepth(uint8_t depth) { mqttQueueDepth = depth; }
void HealthMonitor::setSseClientCount(uint8_t count) { sseClientCount = count; }
void HealthMonitor::setPollDurationMs(uint32_t ms)   { pollDurationMs = ms; }

void HealthMonitor::setBootTime(unsigned long bootMs) {
    currentHealth.bootTimeMs = bootMs;
}

// ── 内存池注册与监控（碎片预防）──────────────────────────────────────────
void HealthMonitor::registerPool(const PoolStatsEntry& entry) {
    if (_trackedPoolCount >= MAX_TRACKED_POOLS) {
        LOG_WARNING("[HealthMonitor] Pool registry full, cannot register more pools");
        return;
    }
    _trackedPools[_trackedPoolCount++] = entry;
    char buf[64];
    snprintf(buf, sizeof(buf), "[HealthMonitor] Pool registered: %s (%uB × %u)",
             entry.name, (unsigned)entry.blockSize, (unsigned)entry.capacity);
    LOG_INFO(buf);
}

void HealthMonitor::logPoolStats() {
    for (uint8_t i = 0; i < _trackedPoolCount; i++) {
        const PoolStatsEntry& p = _trackedPools[i];
        uint8_t used = p.getUsed ? p.getUsed() : 0;
        uint32_t exhaust = p.getExhaust ? p.getExhaust() : 0;
        if (used > 0 || exhaust > 0) {
            char buf[96];
            snprintf(buf, sizeof(buf), "[Pool] %s: used=%u/%u exhaust_count=%lu",
                     p.name, (unsigned)used, (unsigned)p.capacity, (unsigned long)exhaust);
            LOG_INFO(buf);
        }
    }
}

// ── DRAM 水位趋势泄漏检测 ──────────────────────────────────────────────
// 比较前一半采样与后一半采样的平均水位，如果后半段显著低于前半段（>8KB），
// 表明 DRAM 有持续下降趋势，可能存在内存泄漏。
bool HealthMonitor::detectMemoryLeak() const {
    if (_dramHistoryCount < 6) return false;

    // 按时间顺序遍历环形缓冲区，分为前半和后半
    uint8_t n = _dramHistoryCount;
    uint8_t half = n / 2;

    // 计算最早 half 个采样的平均值
    uint64_t earlySum = 0;
    for (uint8_t i = 0; i < half; i++) {
        // 环形缓冲区：最早的采样在 (_dramHistoryIndex - n + i) mod SIZE
        uint8_t idx = (_dramHistoryIndex + WATERMARK_HISTORY_SIZE - n + i) % WATERMARK_HISTORY_SIZE;
        earlySum += _dramWatermarkHistory[idx];
    }
    uint32_t earlyAvg = (uint32_t)(earlySum / half);

    // 计算最新 half 个采样的平均值
    uint64_t lateSum = 0;
    for (uint8_t i = half; i < n; i++) {
        uint8_t idx = (_dramHistoryIndex + WATERMARK_HISTORY_SIZE - n + i) % WATERMARK_HISTORY_SIZE;
        lateSum += _dramWatermarkHistory[idx];
    }
    uint32_t lateAvg = (uint32_t)(lateSum / (n - half));

    // 如果后半段平均水位比前半段低 8KB 以上，判定为疑似泄漏
    static constexpr uint32_t LEAK_THRESHOLD_BYTES = 8192;
    if (earlyAvg > lateAvg && (earlyAvg - lateAvg) > LEAK_THRESHOLD_BYTES) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[HealthMonitor] Leak suspect: early_avg=%lu late_avg=%lu drop=%lu over %u hours",
                 (unsigned long)earlyAvg, (unsigned long)lateAvg,
                 (unsigned long)(earlyAvg - lateAvg), (unsigned)n);
        LOG_WARNING(buf);
        return true;
    }
    return false;
}

// ── 返回完整指标 JSON ────────────────────────────────────────────────
String HealthMonitor::getMetricsJson() {
    JsonDocument doc;
    doc["heap"]["free"]          = currentHealth.freeHeap;
    doc["heap"]["min"]           = currentHealth.minFreeHeap;
    doc["heap"]["largest_block"] = currentHealth.largestFreeBlock;
    doc["heap"]["fragmentation"] = currentHealth.heapFragmentation;
    // DRAM 内部内存（排除 PSRAM，用于 WiFi/MQTT/SSL 保护决策）
    doc["heap"]["dram_free"]     = currentHealth.dramFreeHeap;
    doc["heap"]["dram_largest"]  = currentHealth.dramLargestBlock;
    doc["mqtt"]["queue_depth"]   = mqttQueueDepth;
    doc["sse"]["client_count"]   = sseClientCount;
    doc["poll"]["duration_ms"]   = pollDurationMs;
    doc["uptime"]                = currentHealth.uptime / 1000UL;
    doc["boot_time_ms"]           = currentHealth.bootTimeMs;

    // MemGuard 等级信息
    static const char* levelNames[] = { "NORMAL", "WARN", "SEVERE", "CRITICAL" };
    uint8_t lvl = static_cast<uint8_t>(_currentLevel);
    doc["memguard"]["level"]              = lvl;
    doc["memguard"]["level_name"]         = levelNames[lvl < 4 ? lvl : 0];
    doc["memguard"]["fragmentation_high"] = isFragmentationHigh();
    // 内存恢复状态（防砖机制进度）
    doc["memguard"]["mqtts_downgraded"]    = _mqttsMemoryDowngrade;
    doc["memguard"]["mqtt_stopped"]        = _mqttStoppedForMemory;
    doc["memguard"]["mqtt_disabled"]       = _mqttDisabledForMemory;
    doc["memguard"]["modbus_stopped"]      = _modbusStoppedForMemory;
    doc["memguard"]["periph_exec_paused"]  = _periphExecPausedForMemory;
    doc["memguard"]["critical_duration_s"] = (unsigned long)(getCriticalDurationMs() / 1000);

    // DRAM 水位趋势（泄漏检测）
    doc["stability"]["dram_history_count"] = _dramHistoryCount;
    doc["stability"]["leak_suspect"] = (_dramHistoryCount >= 6) ? detectMemoryLeak() : false;
    doc["stability"]["uptime_hours"] = (unsigned long)(currentHealth.uptime / 3600000UL);

    // PSRAM 信息（用于前端判断 MQTTS 支持）
    doc["heap"]["psram_total"] = (unsigned long)(ESP.getPsramSize() / 1024);
    doc["heap"]["psram_free"]  = (unsigned long)(ESP.getFreePsram() / 1024);
    doc["heap"]["tls_supported"] = (psramFound() && ESP.getPsramSize() > 0);

    // 内存池统计（碎片预防监控）
    for (uint8_t i = 0; i < _trackedPoolCount; i++) {
        const PoolStatsEntry& p = _trackedPools[i];
        String key = String("pools.") + p.name;
        doc[key]["used"] = p.getUsed ? p.getUsed() : 0;
        doc[key]["capacity"] = p.capacity;
        doc[key]["exhaust"] = p.getExhaust ? (unsigned long)p.getExhaust() : 0UL;
    }

    String json;
    serializeJson(doc, json);
    return json;
}

// ── 定期串口输出关键指标摘要 ─────────────────────────────────────────
void HealthMonitor::logMetricsSummary() {
    // 同时输出 DRAM 内部内存 vs 全局内存，方便诊断 PSRAM 干扰问题
    Serial.printf("[METRICS] heap=%lu min=%lu dram=%lu dram_blk=%lu total_blk=%lu frag=%d%% mqtt_q=%d sse=%d poll=%lums up=%lus\n",
        (unsigned long)currentHealth.freeHeap,
        (unsigned long)currentHealth.minFreeHeap,
        (unsigned long)currentHealth.dramFreeHeap,
        (unsigned long)currentHealth.dramLargestBlock,
        (unsigned long)currentHealth.largestFreeBlock,
        (int)currentHealth.heapFragmentation,
        (int)mqttQueueDepth,
        (int)sseClientCount,
        (unsigned long)pollDurationMs,
        (unsigned long)(currentHealth.uptime / 1000UL));
    // 输出内存池使用率（碎片预防）
    logPoolStats();
}

size_t HealthMonitor::getTaskStackInfo(TaskStackInfo* outInfo, size_t maxTasks) const {
    const char* taskNames[] = { "loopTask", "async_tcp", "mqtt_reconn", "IDLE0", "IDLE1" };
    size_t count = 0;
    for (size_t i = 0; i < 5 && count < maxTasks; i++) {
        TaskHandle_t h = xTaskGetHandle(taskNames[i]);
        if (h) {
            strncpy(outInfo[count].name, taskNames[i], sizeof(outInfo[count].name) - 1);
            outInfo[count].name[sizeof(outInfo[count].name) - 1] = '\0';
            outInfo[count].highWaterMark = uxTaskGetStackHighWaterMark(h) * 4;  // 转换为字节
            count++;
        }
    }
    return count;
}

// ── 内存恢复：SEVERE 级别服务降级 ─────────────────────────────────────
void HealthMonitor::performMemoryRecovery(MemoryGuardLevel oldLevel, MemoryGuardLevel newLevel) {
    uint32_t dramBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // 使用已有的碎片率计算函数（基于 DRAM），避免 uint8_t 截断和乘法溢出风险
    uint8_t fragPercent = (dramBefore > 0)
        ? calculateHeapFragmentationPercent(currentHealth.dramFreeHeap, currentHealth.dramLargestBlock)
        : 0;
    Serial.printf("[MEMRECOVER] === SEVERE recovery triggered === dram=%lu largest=%lu frag=%d%%\n",
                  (unsigned long)dramBefore,
                  (unsigned long)currentHealth.dramLargestBlock,
                  (int)fragPercent);

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;

    // Step 1: MQTTS → MQTT 降级（释放 ~30-50KB TLS DRAM）
#if FASTBEE_ENABLE_MQTT
    if (pm && !_mqttsMemoryDowngrade) {
        MQTTClient* mqtt = pm->getMQTTClient();
        if (mqtt && mqtt->getConfig().scheme == "mqtts") {
            Serial.println("[MEMRECOVER] Step 1/3: MQTTS -> MQTT downgrade (releasing ~35KB TLS DRAM)");
            downgradeMqttsToMqtt();
        }
    }
#endif

    // Step 2: 停止 Modbus RTU（释放串口缓冲 + 停止轮询 CPU 开销）
#if FASTBEE_ENABLE_MODBUS
    if (pm && !_modbusStoppedForMemory) {
        ModbusHandler* modbus = pm->getModbusHandler();
        if (modbus && modbus->isRunning()) {
            Serial.println("[MEMRECOVER] Step 2/3: Modbus stopped (was running)");
            pm->stopModbus();
            _modbusStoppedForMemory = true;
        }
    }
#endif

    // Step 3: 暂停 PeriphExec 定时器（减少 CPU 和内存峰值）
    if (!_periphExecPausedForMemory) {
        PeriphExecManager& execMgr = PeriphExecManager::getInstance();
        PeriphExecScheduler* scheduler = execMgr.getScheduler();
        if (scheduler) {
            Serial.println("[MEMRECOVER] Step 3/3: PeriphExec timers paused");
            scheduler->setMemoryPaused(true);
            _periphExecPausedForMemory = true;
        }
    }

    uint32_t dramAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[MEMRECOVER] === SEVERE recovery complete === dram=%lu (gained %ld bytes)\n",
                  (unsigned long)dramAfter, (long)(dramAfter - dramBefore));
}

// ── MQTTS → MQTT 降级：修改配置文件并重启 MQTT ───────────────────────
void HealthMonitor::downgradeMqttsToMqtt() {
    Serial.println("[MEMRECOVER]   protocol.json updating: scheme=mqtt, port=1883");

    // 写入配置文件：scheme=mqtt, port=1883
    const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE;
    bool configUpdated = HandlerUtils::updateJsonConfig(configPath,
        [](JsonDocument& doc) {
            if (doc["mqtt"].is<JsonObject>()) {
                doc["mqtt"]["scheme"] = "mqtt";
                doc["mqtt"]["port"] = 1883;
            }
        });

    if (!configUpdated) {
        Serial.println("[MEMRECOVER]   WARN: failed to update protocol.json, skip MQTT restart");
        return;  // 配置写入失败，不标记已降级，不重启 MQTT
    }
    Serial.println("[MEMRECOVER]   protocol.json updated: scheme=mqtt, port=1883");

    // 标记已降级（防止重复操作）
    _mqttsMemoryDowngrade = true;

    // 停止当前 MQTT（释放 TLS 传输层）并延迟重启（使用新配置 mqtt://）
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
    if (pm) {
        pm->stopMQTT();
        // 延迟重启：配置文件已更新为 mqtt，重启后使用非加密连接
        pm->restartMQTTDeferred();
        Serial.println("[MEMRECOVER]   MQTT stopped + deferred restart with plain scheme");
    }

    // 记录 DRAM 变化用于诊断
    uint32_t dramAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    Serial.printf("[MEMRECOVER]   DRAM after downgrade: %lu bytes\n", (unsigned long)dramAfter);
}

// ── NORMAL 恢复：重启因内存停止的服务 ────────────────────────────────
void HealthMonitor::restoreMemoryRecovery() {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;

    Serial.println("[MEMRECOVER] === NORMAL: restoring services ===");

    // 恢复 Modbus（如果被内存停止过）
#if FASTBEE_ENABLE_MODBUS
    if (_modbusStoppedForMemory && pm) {
        pm->restartModbusDeferred();
        _modbusStoppedForMemory = false;
        Serial.println("[MEMRECOVER]   Modbus restart deferred");
    }
#endif

    // 恢复 PeriphExec 定时器
    if (_periphExecPausedForMemory) {
        PeriphExecManager& execMgr = PeriphExecManager::getInstance();
        PeriphExecScheduler* scheduler = execMgr.getScheduler();
        if (scheduler) {
            scheduler->setMemoryPaused(false);
        }
        _periphExecPausedForMemory = false;
        Serial.println("[MEMRECOVER]   PeriphExec timers resumed");
    }

    // 恢复 MQTT：被禁用时不自动恢复，用户需手动在 Web 界面开启
#if FASTBEE_ENABLE_MQTT
    if (_mqttDisabledForMemory) {
        Serial.println("[MEMRECOVER]   MQTT stays DISABLED (config: enabled=false), user must re-enable manually");
        // 不重置 _mqttDisabledForMemory，不重启 MQTT
    } else if (_mqttStoppedForMemory && pm) {
        pm->restartMQTTDeferred();
        _mqttStoppedForMemory = false;
        Serial.println("[MEMRECOVER]   MQTT restart deferred");
    }
#endif

    // 重置 CRITICAL 计时
    _criticalStartTime = 0;

    Serial.println("[MEMRECOVER] === NORMAL restore complete ===");
}

// ── 永久禁用 MQTT：写入配置 + 停止（CRITICAL 30s 最后手段）────────────
void HealthMonitor::disableMqttForMemory() {
    Serial.println("[MEMRECOVER]   Disabling MQTT in config (memory critical)");
    const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE;
    bool ok = HandlerUtils::updateJsonConfig(configPath,
        [](JsonDocument& doc) {
            if (doc["mqtt"].is<JsonObject>()) {
                doc["mqtt"]["enabled"] = false;
            }
        });
    if (!ok) {
        Serial.println("[MEMRECOVER]   WARN: failed to disable MQTT in config");
        return;
    }
    Serial.println("[MEMRECOVER]   protocol.json updated: mqtt.enabled=false");

    // 立即停止 MQTT（配置已更新，重启后也不会重连）
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
    if (pm) {
        pm->stopMQTT();
        Serial.println("[MEMRECOVER]   MQTT stopped (will NOT reconnect after reboot)");
    }
}
