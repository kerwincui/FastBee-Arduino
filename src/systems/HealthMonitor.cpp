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
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <core/SystemConstants.h>
#include <core/FeatureFlags.h>
#if FASTBEE_ENABLE_MQTT
#include "core/FastBeeFramework.h"
#include "protocols/ProtocolManager.h"
#include "protocols/MQTTClient.h"
#endif

HealthMonitor::HealthMonitor()
    : lastCheckTime(0),
      lastStackLogTime(0),
      lastMetricsLogTime(0),
      heapWatermark(UINT32_MAX),
      consecutiveLowMemCount(0),
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
    currentHealth.freeHeap    = esp_get_free_heap_size();
    currentHealth.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    if (currentHealth.freeHeap < heapWatermark) {
        heapWatermark = currentHealth.freeHeap;
    }

    // 碎片率 = 1 - (最大连续块 / 总空闲)
    size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    currentHealth.largestFreeBlock = largestBlock;
    currentHealth.heapFragmentation = (currentHealth.freeHeap > 0)
        ? static_cast<uint8_t>(100U - (largestBlock * 100U / currentHealth.freeHeap))
        : 0;

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

// 低内存保护：连续多次检测到严重低内存时重启设备
void HealthMonitor::checkCriticalMemory() {
    // 严重低内存阈值：8KB 以下很可能导致崩溃
    constexpr uint32_t CRITICAL_HEAP_THRESHOLD = 8192;
    // 危险低内存阈值：16KB 以下开始警告
    constexpr uint32_t WARNING_HEAP_THRESHOLD = 16384;
    // 连续严重低内存次数阈值：连续 3 次（15秒）就重启
    constexpr uint32_t CRITICAL_COUNT_THRESHOLD = 3;

    if (currentHealth.freeHeap < CRITICAL_HEAP_THRESHOLD) {
        consecutiveLowMemCount++;
        char buf[96];
        snprintf(buf, sizeof(buf), "Health: CRITICAL low memory! heap=%lu maxBlock=%lu count=%lu",
                 (unsigned long)currentHealth.freeHeap,
                 (unsigned long)currentHealth.largestFreeBlock,
                 (unsigned long)consecutiveLowMemCount);
        LOG_ERROR(buf);

        if (consecutiveLowMemCount >= CRITICAL_COUNT_THRESHOLD) {
            LOG_ERROR("Health: Memory critically low for too long, rebooting!");
            delay(100);
            ESP.restart();
        }
    } else if (currentHealth.freeHeap < WARNING_HEAP_THRESHOLD) {
        consecutiveLowMemCount = 0;
        char buf[80];
        snprintf(buf, sizeof(buf), "Health: Low memory warning heap=%lu maxBlock=%lu",
                 (unsigned long)currentHealth.freeHeap,
                 (unsigned long)currentHealth.largestFreeBlock);
        LOG_WARNING(buf);
    } else {
        consecutiveLowMemCount = 0;
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
    uint32_t freeHeap = currentHealth.freeHeap;
    uint32_t largestBlock = currentHealth.largestFreeBlock;
    MemoryGuardLevel newLevel;

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

    // 基于 largestFreeBlock 的强制提升：即使 freeHeap 看起来正常，
    // 如果最大可分配块过小，Web 服务仍无法响应请求
    if (largestBlock < 4096) {
        // 最大块 < 4KB：几乎无法分配任何 HTTP 响应缓冲区
        if (newLevel != MemoryGuardLevel::CRITICAL) {
            Serial.printf("[MEMGUARD] largestFreeBlock=%lu < 4KB, elevating to CRITICAL\n",
                          (unsigned long)largestBlock);
            newLevel = MemoryGuardLevel::CRITICAL;
        }
    } else if (largestBlock < 8192) {
        // 最大块 < 8KB：只能响应小请求
        if (newLevel < MemoryGuardLevel::SEVERE) {
            Serial.printf("[MEMGUARD] largestFreeBlock=%lu < 8KB, elevating to SEVERE\n",
                          (unsigned long)largestBlock);
            newLevel = MemoryGuardLevel::SEVERE;
        }
    }

    // 碎片率过高时至少提升到 WARN，若 largestFreeBlock 也小则进一步提升
    if (isFragmentationHigh()) {
        if (newLevel == MemoryGuardLevel::NORMAL) {
            newLevel = MemoryGuardLevel::WARN;
            Serial.printf("[MEMGUARD] High fragmentation (%d%%), elevating to WARN\n",
                          (int)currentHealth.heapFragmentation);
        } else if (newLevel == MemoryGuardLevel::WARN && largestBlock < 8192) {
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
        }
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
    char logBuf[96];
    snprintf(logBuf, sizeof(logBuf),
             "[HealthMonitor] High fragmentation (%d%%), triggering compaction",
             (int)currentHealth.heapFragmentation);
    LOG_WARNING(logBuf);

    // 策略1: 释放日志缓冲区（如果 LoggerSystem 提供了相关接口）
    // LoggerSystem 的文件写入缓冲在低内存时已由 MemGuard SEVERE 级别停止

    // 策略2: 记录碎片化事件用于诊断
    Serial.printf("[MEMGUARD] Compaction triggered: frag=%d%% freeHeap=%lu largestBlock=%lu\n",
                  (int)currentHealth.heapFragmentation,
                  (unsigned long)currentHealth.freeHeap,
                  (unsigned long)currentHealth.largestFreeBlock);

    // 策略3: 尝试通过分配+释放一个较大块来促使堆合并相邻空闲块
    // 注意：ESP32 没有直接的内存紧凑化 API，这只是一个尽力而为的尝试
    size_t trySize = currentHealth.largestFreeBlock / 2;
    if (trySize > 1024) {
        void* tmp = malloc(trySize);
        if (tmp) {
            free(tmp);
        }
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
    if (currentHealth.freeHeap == 0) return false;
    uint8_t frag = 100 - (currentHealth.largestFreeBlock * 100 / currentHealth.freeHeap);
    return frag > FRAG_THRESHOLD_COMPACT;
}

// ── 外部模块上报指标 setter ──────────────────────────────────────────
void HealthMonitor::setMqttQueueDepth(uint8_t depth) { mqttQueueDepth = depth; }
void HealthMonitor::setSseClientCount(uint8_t count) { sseClientCount = count; }
void HealthMonitor::setPollDurationMs(uint32_t ms)   { pollDurationMs = ms; }

void HealthMonitor::setBootTime(unsigned long bootMs) {
    currentHealth.bootTimeMs = bootMs;
}

// ── 返回完整指标 JSON ────────────────────────────────────────────────
String HealthMonitor::getMetricsJson() {
    JsonDocument doc;
    doc["heap"]["free"]          = currentHealth.freeHeap;
    doc["heap"]["min"]           = currentHealth.minFreeHeap;
    doc["heap"]["largest_block"] = currentHealth.largestFreeBlock;
    doc["heap"]["fragmentation"] = currentHealth.heapFragmentation;
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

    String json;
    serializeJson(doc, json);
    return json;
}

// ── 定期串口输出关键指标摘要 ─────────────────────────────────────────
void HealthMonitor::logMetricsSummary() {
    Serial.printf("[METRICS] heap=%lu min=%lu largest=%lu frag=%d%% mqtt_q=%d sse=%d poll=%lums up=%lus\n",
        (unsigned long)currentHealth.freeHeap,
        (unsigned long)currentHealth.minFreeHeap,
        (unsigned long)currentHealth.largestFreeBlock,
        (int)currentHealth.heapFragmentation,
        (int)mqttQueueDepth,
        (int)sseClientCount,
        (unsigned long)pollDurationMs,
        (unsigned long)(currentHealth.uptime / 1000UL));
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
