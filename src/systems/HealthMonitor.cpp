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
#include "core/FeatureBudget.h"
#include "core/RuntimeFeatureManager.h"
#include "core/ChipConfig.h"
#include "systems/LoggerSystem.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <cstring>
#include <core/SystemConstants.h>
#include <core/FeatureFlags.h>
#include "core/FastBeeFramework.h"
#include "core/PeriphExecManager.h"
#include "core/PeriphExecScheduler.h"
#include "network/handlers/HandlerUtils.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeripheralManager.h"
#endif
#include "network/WebMemoryGuard.h"  // 定义 FASTBEE_WEB_MEM_GUARD
#if FASTBEE_WEB_MEM_GUARD
#include "network/WebConfigManager.h"
#endif
// === Phase B Task B.3: MemoryProfileDB 集成 === (Disabled)
// #include "core/MemoryProfile.h"
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
#if FASTBEE_ENABLE_STORAGE_CACHE || FASTBEE_ENABLE_MQTT
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
      criticalThreshold(0),
      warningThreshold(0),
      _compactionTriggered(false),
      mqttQueueDepth(0),
      sseClientCount(0),
      pollDurationMs(0) {
    memset(&currentHealth, 0, sizeof(currentHealth));
}

// Restore only actions explicitly marked as automatic memory protection.
// A plain mqtt.enabled=false written by a user must remain a normal setting.
void HealthMonitor::loadPersistedMqttsDowngrade() {
#if FASTBEE_ENABLE_MQTT
    JsonDocument doc;
#if FASTBEE_ENABLE_STORAGE_CACHE || FASTBEE_ENABLE_MQTT
    if (!ConfigStorage::getInstance().loadProtocolSection("mqtt", doc)) {
        return;
    }
#else
    return;
#endif

    JsonObjectConst mqtt = doc["mqtt"];
    if (mqtt.isNull()) {
        return;
    }

    // Keep the automatic memory action distinguishable from a normal user
    // setting.  Older builds used the longer alias; accept it during upgrade
    // so a one-time firmware update does not lose the protection state.
    const bool disabledByMemory =
        (mqtt["disabled_by_memory"] | false) ||
        (mqtt["disable_mqtt_by_memory"] | false);
    if (disabledByMemory) {
        _mqttDisabledForMemory = true;
        _mqttStoppedForMemory = true;
        Serial.println("[MEMRECOVER] restored persisted MQTT disable (memory pressure)");
    }

    if (!(mqtt["downgraded_by_memory"] | false)) {
        return;
    }

    _mqttsMemoryDowngrade = true;
    _mqttsDowngradeTimeMs = millis();

    String originalScheme = mqtt["downgraded_original_scheme"] | "mqtts";
    if (originalScheme.isEmpty()) originalScheme = "mqtts";
    strncpy(_mqttsOriginalScheme, originalScheme.c_str(), sizeof(_mqttsOriginalScheme) - 1);
    _mqttsOriginalScheme[sizeof(_mqttsOriginalScheme) - 1] = '\0';

    const uint32_t originalPort = mqtt["downgraded_original_port"] | 8883U;
    _mqttsOriginalPort = (originalPort > 0 && originalPort <= 65535U)
        ? static_cast<uint16_t>(originalPort) : 8883;

    // RuntimeFeatureManager is loaded before HealthMonitor during boot. Keep
    // the persisted downgrade visible in /api/features/status immediately,
    // while still leaving features.json and the user's setting untouched.
    FastBee::RuntimeFeatureManager::instance().setRuntimeDisabled(
        "mqtts", "内存压力：TLS 暂时降级为 MQTT");

    Serial.printf("[MEMRECOVER] restored persisted MQTTS downgrade (original=%s:%u)\n",
                  _mqttsOriginalScheme, (unsigned)_mqttsOriginalPort);
#endif
}

void HealthMonitor::clearMqttMemoryOverrideState() {
#if FASTBEE_ENABLE_MQTT
    // The protocol route has already persisted the user's explicit choice and
    // removed the on-disk markers.  Clear only runtime bookkeeping here; the
    // next health sample may apply a fresh downgrade again if pressure is
    // still genuinely present.
    const bool hadMemoryOverride = _mqttsMemoryDowngrade ||
                                   _mqttDisabledForMemory ||
                                   _mqttStoppedForMemory ||
                                   _mqttRestorePendingForMemory;
    _mqttsMemoryDowngrade = false;
    _mqttDisabledForMemory = false;
    _mqttStoppedForMemory = false;
    _mqttsDowngradeRetryAtMs = 0;
    _mqttsRestoreRetryAtMs = 0;
    _mqttsRestoreCooldownMs = 0;
    _mqttDisableRetryAtMs = 0;
    _mqttRestorePendingForMemory = false;
    _mqttRestoreRetryAtMs = 0;
    // A hot-restart may be deferred or intentionally skipped when the
    // existing client is still usable. Clear its in-memory marker as well as
    // the persisted flags so SSE does not keep reporting a stale protection
    // decision after the user explicitly saves MQTT settings.
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
    if (pm && pm->getMQTTClient()) {
        pm->getMQTTClient()->setDisabledByMemory(false);
        pm->getMQTTClient()->setStoppedByMemory(false);
    }
    if (hadMemoryOverride) {
        FastBee::RuntimeFeatureManager::instance().clearRuntimeDisabled("mqtts");
        Serial.println("[MEMRECOVER] cleared MQTT memory override after explicit Web configuration");
    }
#endif
}

bool HealthMonitor::initialize() {
    running = true;
    // A previous boot may have persisted a TLS->plain-MQTT downgrade. Load
    // that marker before the first health sample so the state API and the
    // recovery path remain truthful across restarts.
    loadPersistedMqttsDowngrade();
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
        reconcilePendingMemoryRecovery();
        checkCriticalMemory();
#if FASTBEE_WEB_MEM_GUARD
        // 周期推进 Web 护栏状态机（与请求到达解耦）：2026-08-24 OOM 死锁
        // 取证——客户端被 503 击退停止轮询后中间件不再运行，FORCE-RECOVER
        // 兜底永不触发，护栏永久锁死。此处 5s 周期调用保证无请求时
        // 状态机仍持续推进。
        {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            WebConfigManager* wcm = fw ? fw->getWebConfigManager() : nullptr;
            WebMemoryGuardMiddleware* guard = wcm ? wcm->getMemGuard() : nullptr;
            if (guard) guard->maintain();
        }
#endif
        
        // === Phase B Task B.3: 功能内存剖绘采样器 (30 秒间隔) === (Disabled)
        // static unsigned long lastSampleTime = 0;
        // if (currentTime - lastSampleTime >= 30000UL) {
        //     FastBee::FunctionMemoryProfiler::instance().runFullScan();
        //     lastSampleTime = currentTime;
        // }
        
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
    // ── DRAM 8BIT 口径（真实可 malloc 内存，排除 PSRAM/IRAM/RTC）──────────
    currentHealth.dramFreeHeap     = FastBee::MemoryBudget::dram8Free();
    currentHealth.dramLargestBlock = FastBee::MemoryBudget::dram8Largest();

    // ── 内存状态（8BIT DRAM 口径，真实可 malloc 内存）─────────────────
    // 统一使用 MemoryBudget 的内部 8-bit DRAM 口径，避免裸 MALLOC_CAP_8BIT
    // 在 PSRAM 板型上把外部内存计入，导致降级触发滞后。
    currentHealth.freeHeap    = FastBee::MemoryBudget::dram8Free();
    currentHealth.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

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

    // 碎片率 = 1 - (最大连续块 / 总空闲)，基于 8BIT DRAM 数据（排除 PSRAM 干扰）
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
    
    // === Phase C Task C.2: 碎片率严重度评估 ===
    float fragRatio = (float)currentHealth.dramLargestBlock / 
                      (float)currentHealth.dramFreeHeap;
    
    currentHealth.fragmentationSeverity = 0;  // 健康
    if (fragRatio < CHIP_FRAGMENTATION_CRITICAL_RATIO) {
        currentHealth.fragmentationSeverity = 3;  // 危急
    } else if (fragRatio < CHIP_FRAGMENTATION_SUSPEND_RATIO) {
        currentHealth.fragmentationSeverity = 2;  // 警告
    } else if (fragRatio < 0.50f) {
        currentHealth.fragmentationSeverity = 1;  // 轻微
    }
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

void HealthMonitor::initializeMemoryThresholds() {
    static bool thresholdsInitialized = false;
    static uint32_t criticalThresholdCache = 0;
    static uint32_t warningThresholdCache = 0;
    static uint32_t guardWarnDramCache = FastBee::MemoryBudget::GUARD_WARN_DRAM_FREE;
    static uint32_t guardSevereDramCache = FastBee::MemoryBudget::GUARD_SEVERE_DRAM_FREE;
    static uint32_t guardCriticalLargestCache = FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK;
    static uint32_t guardSevereLargestCache = FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK;
    static uint32_t guardWarnLargestCache = FastBee::MemoryBudget::GUARD_WARN_LARGEST_BLOCK;

    if (!thresholdsInitialized) {
        const size_t psramSize = ESP.getPsramSize();

        if (psramSize >= 8 * 1024 * 1024) {
            criticalThresholdCache = 12288U;  // 12KB, F16R8
            warningThresholdCache  = 24576U;  // 24KB
        } else if (psramSize >= 4 * 1024 * 1024) {
            criticalThresholdCache = 10240U;  // 10KB, custom 4MB PSRAM
            warningThresholdCache  = 20480U;  // 20KB
        } else {
            criticalThresholdCache = 8192U;   // 8KB, F8R0/C3/C6
            warningThresholdCache  = 16384U;  // 16KB
        }

        criticalThresholdCache = constrain(criticalThresholdCache, 4096U, 16384U);
        warningThresholdCache  = constrain(warningThresholdCache, 8192U, 32768U);

#if CHIP_IS_CLASSIC_ESP32
        // The full F4R0 image has a measured 14~15KB internal-DRAM idle
        // baseline.  The project-wide 24/30KB bands classify that healthy
        // state as SEVERE and stop PeriphExec, even though the web-reserve
        // policy still has enough room to serve requests.  Keep the genuine
        // critical free-heap floor at 8KB while using 12/20KB free-heap bands
        // for this no-PSRAM profile.  The shared 3/4/8KB contiguous-block
        // bands match the actual Web/Modbus/PeriphExec allocation floors.
        if (psramSize == 0) {
            guardWarnDramCache = 20480U;
            guardSevereDramCache = 12288U;
            guardCriticalLargestCache = FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK;
            guardSevereLargestCache = FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK;
            guardWarnLargestCache = FastBee::MemoryBudget::GUARD_WARN_LARGEST_BLOCK;
        }
#elif CHIP_IS_ESP32C3
        // C3 uses the same no-PSRAM DRAM pool as the classic F4R0 profile.
        // A transient web/MQTT burst can leave a 7~12KB largest block while
        // total free8 remains 30~40KB. Keep the genuine free8<8KB emergency
        // floor and the measured no-PSRAM free-heap bands. Contiguous-block
        // thresholds use the shared service allocation floors above.
        if (psramSize == 0) {
            guardWarnDramCache = 20480U;
            guardSevereDramCache = 12288U;
            guardCriticalLargestCache = FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK;
            guardSevereLargestCache = FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK;
            guardWarnLargestCache = FastBee::MemoryBudget::GUARD_WARN_LARGEST_BLOCK;
        }
#endif

        thresholdsInitialized = true;

        LOGGER.info(String("[THRESHOLD] Initialized for ") + CHIP_NAME +
                    String(" with PSRAM=") + (unsigned long)(psramSize / 1024) +
                    String("KB: critical=") + (unsigned int)(criticalThresholdCache / 1024) +
                    String("KB, warn=") + (unsigned int)(warningThresholdCache / 1024) +
                    String("KB, guard=") + (unsigned int)(guardSevereDramCache / 1024) +
                    "/" + (unsigned int)(guardWarnDramCache / 1024) + "KB");
    }

    // Copy the cached policy for every instance; source-level tests construct
    // more than one monitor in a process even though firmware uses a singleton.
    criticalThreshold = criticalThresholdCache;
    warningThreshold = warningThresholdCache;
    guardWarnDramThreshold = guardWarnDramCache;
    guardSevereDramThreshold = guardSevereDramCache;
    guardCriticalLargestThreshold = guardCriticalLargestCache;
    guardSevereLargestThreshold = guardSevereLargestCache;
    guardWarnLargestThreshold = guardWarnLargestCache;
}

// 低内存保护：分级恢复机制（替代原有的 15s 硬重启）
// EMERGENCY: DRAM < 4KB → 立即禁用 MQTT + 停止所有非 Web 服务
// CRITICAL 持续 30s → 写入配置禁用 MQTT（重启后也不连）
// CRITICAL 持续 90s → 安全重启（boot loop 保护：启动 < 120s 不重启）
void HealthMonitor::checkCriticalMemory() {
    initializeMemoryThresholds();

    // 使用 8BIT DRAM 进行检测（排除 PSRAM/IRAM/RTC，真实可 malloc 内存）
    // WiFi/MQTT/SSL 都不能用 PSRAM，使用 DRAM 空闲来评估真实风险
    const uint32_t checkHeap = currentHealth.dramFreeHeap;

    // ── 紧急兜底：DRAM < 4KB，系统随时可能 crash，不等计时立即执行所有恢复 ──
    // 首次进入时记录时间；恢复无效且持续低于阈值则快速优雅重启。
    // 背景（2026-08-24 .34 实测）：DRAM 29KB→700B 仅需 ~40s，EMERGENCY 恢复
    // gained 0 bytes，90s CRITICAL 重启计时远水不救近火，2s 内即 new() 失败 abort
    // （rst:0xc SW_CPU_RESET，panic 无 RTC 快照，现场极难取证）。
    static constexpr uint32_t EMERGENCY_DRAM_THRESHOLD = 4096;
    if (checkHeap < EMERGENCY_DRAM_THRESHOLD) {
        if (_emergencySince == 0) {
            _emergencySince = millis();
            Serial.printf("[MEMRECOVER] EMERGENCY: DRAM=%lu < 4KB, immediate full recovery\n",
                          (unsigned long)checkHeap);
            // 立即执行 SEVERE 恢复（如果还没做）
            uint32_t dramBeforeRecovery = FastBee::MemoryBudget::dram8Free();
            performMemoryRecovery(MemoryGuardLevel::NORMAL, MemoryGuardLevel::CRITICAL);
            // 立即禁用 MQTT（幂等）
            _mqttDisabledForMemory = true;
            _mqttStoppedForMemory = true;
            disableMqttForMemory();
            uint32_t dramAfterRecovery = FastBee::MemoryBudget::dram8Free();
            Serial.printf("[MEMRECOVER] EMERGENCY complete: DRAM=%lu (gained %ld bytes), Web stays available\n",
                          (unsigned long)dramAfterRecovery,
                          (long)dramAfterRecovery - (long)dramBeforeRecovery);
        } else {
            // 恢复已执行过但 DRAM 仍 < 4KB：留给短暂尖峰 5s 自愈窗口，
            // 否则抢在任何 new() 失败 abort 之前优雅重启（保留 RTC 快照可取证）。
            unsigned long emergencyDuration = millis() - _emergencySince;
            if (emergencyDuration >= EMERGENCY_REBOOT_DELAY_MS &&
                !SystemRebooter::isScheduled()) {
                if (millis() < 120000UL) {
                    static unsigned long lastEmergencyBootLoopWarn = 0;
                    if (millis() - lastEmergencyBootLoopWarn > 30000UL) {
                        Serial.println("[MEMRECOVER] Skip EMERGENCY reboot: recent boot (<120s), possible boot loop.");
                        lastEmergencyBootLoopWarn = millis();
                    }
                } else {
                    Serial.printf("[MEMRECOVER] EMERGENCY stuck %lus (DRAM=%lu largest=%lu), "
                                  "recovery ineffective, rebooting before OOM abort\n",
                                  (unsigned long)(emergencyDuration / 1000),
                                  (unsigned long)checkHeap,
                                  (unsigned long)currentHealth.dramLargestBlock);
                    char reasonBuf[48];
                    snprintf(reasonBuf, sizeof(reasonBuf), "DRAM<4KB stuck %lus",
                             (unsigned long)(emergencyDuration / 1000));
                    SystemRebooter::scheduleReboot(reasonBuf, 2000UL, RestartReason::CRITICAL_LOW_MEMORY);
                }
            }
        }
    } else {
        _emergencySince = 0;  // DRAM 回到 4KB 以上，复位 EMERGENCY 计时
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
        if (criticalDuration >= CRITICAL_MQTT_STOP_DELAY_MS && !_mqttDisabledForMemory &&
            (_mqttDisableRetryAtMs == 0 || millis() >= _mqttDisableRetryAtMs)) {
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
            Serial.printf("[MEMRECOVER]   Actions taken: %s%s%s%s%s\n",
                          _mqttsMemoryDowngrade ? "MQTTS->MQTT(SEVERE) " : "",
                          _modbusStoppedForMemory ? "Modbus stopped " : "",
                          _periphExecPausedForMemory ? "PeriphExec paused " : "",
                          _optionalPeripheralsSuspendedForMemory ? "Optional peripherals suspended " : "",
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
    // 扩展覆盖：常驻任务 + worker 池（pexec_w0/w1）+ IDLE
    // 注意：fb_login/ota_url 为瞬态任务，仅在有 handle 时测量
    const char* names[] = {
        "loopTask", "async_tcp", "mqtt_reconn",
        "pexec_w0", "pexec_w1", "fb_login", "ota_url"
    };
    static constexpr int TASK_COUNT = sizeof(names) / sizeof(names[0]);
    TaskHandle_t tasks[TASK_COUNT];

    for (int i = 0; i < TASK_COUNT; i++) {
        tasks[i] = xTaskGetHandle(names[i]);
        if (tasks[i]) {
            // ESP-IDF's FreeRTOS port reports HWM directly in bytes.
            UBaseType_t watermarkBytes = uxTaskGetStackHighWaterMark(tasks[i]);
            if (watermarkBytes < 512) {
                char buf[96];
                snprintf(buf, sizeof(buf), "Health: Task '%s' stack watermark CRITICALLY LOW: %u bytes",
                         names[i], (unsigned)watermarkBytes);
                LOG_ERROR(buf);
            } else if (watermarkBytes < 1024) {
                char buf[96];
                snprintf(buf, sizeof(buf), "Health: Task '%s' stack watermark LOW: %u bytes",
                         names[i], (unsigned)watermarkBytes);
                LOGGER.warning(buf);
            } else {
                char buf[80];
                snprintf(buf, sizeof(buf), "Health: Task '%s' stack watermark: %u bytes",
                         names[i], (unsigned)watermarkBytes);
                LOG_DEBUG(buf);
            }
        }
    }
}

// ── MemGuard 等级判断 ────────────────────────────────────────────────

void HealthMonitor::updateMemoryGuardLevel() {
    initializeMemoryThresholds();

    uint32_t freeHeap = currentHealth.dramFreeHeap;
    // 使用 DRAM 内部连续块做等级判断（排除 PSRAM 干扰）
    // MALLOC_CAP_DEFAULT 在有 PSRAM 的设备上会包含 PSRAM 的巨大连续块（如 8MB）
    // 导致 CRITICAL 判断永远不会触发，实际 DRAM 被消耗却检测不到
    uint32_t largestBlock = currentHealth.dramLargestBlock;  // DRAM 内部连续块
    // Keep the legacy/default classification call as the baseline, then apply
    // the profile-specific runtime bands when this target has one.
    MemoryGuardLevel newLevel = static_cast<MemoryGuardLevel>(static_cast<uint8_t>(
        FastBee::MemoryBudget::guardLevelForFreeDram(freeHeap, criticalThreshold)));
    if (guardSevereDramThreshold != FastBee::MemoryBudget::GUARD_SEVERE_DRAM_FREE ||
        guardWarnDramThreshold != FastBee::MemoryBudget::GUARD_WARN_DRAM_FREE) {
        newLevel = static_cast<MemoryGuardLevel>(static_cast<uint8_t>(
            FastBee::MemoryBudget::guardLevelForFreeDram(
                freeHeap, criticalThreshold, guardSevereDramThreshold,
                guardWarnDramThreshold)));
    }

    // 启动期 grace period：前 90 秒为框架 + lwip + AsyncTCP + NTP + 日志 flush 等
    // 并发初始化阶段，ESP32 no-PSRAM 堆会出现一次性高碎片/小 largestBlock 峰值。
    // 此期间不基于 largestBlock / 碎片率 主动提升到 SEVERE/CRITICAL（仅告警），
    // 避免在系统尚未稳定时误触发永久降级导致主循环卡死。
    // freeHeap 绝对值低的强降级仍然保留——那是真正的危险信号。
    const bool inStartupGrace = (millis() < 90000UL);

    // freeHeap 初始等级使用与 checkCriticalMemory 相同的动态 CRITICAL 下限。
    // 经典 ESP32 完整外设配置的实测稳定基线约 14KB，低于旧固定 16KB
    // 阈值但远高于无 PSRAM 设备真正的 8KB 危险线。

    // 健康反证：freeHeap 虽略低，但 largestBlock 足够大时保持 NORMAL。
    // 连续块门槛与当前芯片档位保持一致，避免经典 ESP32 的 12KB 基线
    // 被通用 16KB 门槛再次抬高。
    if (newLevel == MemoryGuardLevel::WARN && largestBlock >= guardWarnLargestThreshold) {
        newLevel = MemoryGuardLevel::NORMAL;
    }

    // 基于 largestFreeBlock 的强制提升：即使 freeHeap 看起来正常，
    // 如果最大可分配块过小，系统无法工作。
    // 注意：MAX_CONNECTIONS=4 + Connection:close 时正常 web 服务期间 largest 会临时降到数 KB，
    // 因此各档位采用芯片对应的连续块阈值，避免访问网页时误触发降级。
    // 2026-08-24 修复M（菜单 503 根因）：崩溃重启后 blk8 稳态卡在
    // 11252（12288 阈值陷阱带，frag=77%）永久触发 SEVERE，导致
    // /api/protocol/config、/api/users/online 等重响应菜单持续 503，
    // 而此时 freeHeap≈45KB、总堆 maxAlloc=26KB 完全可服务。
    // 补健康反证：freeHeap 达到 NORMAL 阈值（≥30KB）时碎片提升最多到
    // WARN——AsyncTCP 2872B 主缓冲与 JSON 响应小分配仍安全，且
    // rejectHeavyRequestOnPressure 只拦 SEVERE+，菜单不再误伤；
    // freeHeap 也低时保持原提升（真正危险）。
    const bool heapHealthyForFrag = (freeHeap >= guardWarnDramThreshold);
    if (largestBlock < guardCriticalLargestThreshold) {
        // 最大块低于动态 CRITICAL 门限：系统已无法安全分配网络主缓冲
        if (inStartupGrace) {
            static uint32_t lastGraceWarnCrit = 0;
            uint32_t now = millis();
            if (now - lastGraceWarnCrit > 10000UL) {
                lastGraceWarnCrit = now;
                Serial.printf("[MEMGUARD] startup-grace(%lus): largestBlock=%lu<%uB (skip CRITICAL)\n",
                              (unsigned long)(now / 1000), (unsigned long)largestBlock,
                              (unsigned)guardCriticalLargestThreshold);
            }
        } else if (heapHealthyForFrag) {
            static uint32_t lastSkipCritLog = 0;
            uint32_t now = millis();
            if (now - lastSkipCritLog > 60000UL) {
                lastSkipCritLog = now;
                Serial.printf("[MEMGUARD] largestBlock=%lu<%uB but freeHeap=%u healthy, capping at WARN\n",
                              (unsigned long)largestBlock,
                              (unsigned)guardCriticalLargestThreshold,
                              (unsigned)freeHeap);
            }
            if (newLevel < MemoryGuardLevel::WARN) {
                newLevel = MemoryGuardLevel::WARN;
            }
        } else if (newLevel != MemoryGuardLevel::CRITICAL) {
            static uint32_t lastElevCritLog = 0;
            uint32_t now = millis();
            if (now - lastElevCritLog > 60000UL) {
                lastElevCritLog = now;
                Serial.printf("[MEMGUARD] largestFreeBlock=%lu < %uB, elevating to CRITICAL\n",
                              (unsigned long)largestBlock,
                              (unsigned)guardCriticalLargestThreshold);
            }
            newLevel = MemoryGuardLevel::CRITICAL;
        }
    } else if (largestBlock < guardSevereLargestThreshold) {
        // 最大块低于动态 SEVERE 门限：已接近服务自身的连续块下限
        if (inStartupGrace) {
            static uint32_t lastGraceWarnSev = 0;
            uint32_t now = millis();
            if (now - lastGraceWarnSev > 10000UL) {
                lastGraceWarnSev = now;
                Serial.printf("[MEMGUARD] startup-grace(%lus): largestBlock=%lu<%uB (skip SEVERE)\n",
                              (unsigned long)(now / 1000), (unsigned long)largestBlock,
                              (unsigned)guardSevereLargestThreshold);
            }
        } else if (heapHealthyForFrag) {
            static uint32_t lastSkipSevLog = 0;
            uint32_t now = millis();
            if (now - lastSkipSevLog > 60000UL) {
                lastSkipSevLog = now;
                Serial.printf("[MEMGUARD] largestBlock=%lu<%uB but freeHeap=%u healthy, capping at WARN\n",
                              (unsigned long)largestBlock,
                              (unsigned)guardSevereLargestThreshold,
                              (unsigned)freeHeap);
            }
            if (newLevel < MemoryGuardLevel::WARN) {
                newLevel = MemoryGuardLevel::WARN;
            }
        } else if (newLevel < MemoryGuardLevel::SEVERE) {
            static uint32_t lastElevSevLog = 0;
            uint32_t now = millis();
            if (now - lastElevSevLog > 60000UL) {
                lastElevSevLog = now;
                Serial.printf("[MEMGUARD] largestFreeBlock=%lu < %uB, elevating to SEVERE\n",
                              (unsigned long)largestBlock,
                              (unsigned)guardSevereLargestThreshold);
            }
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
        } else if (newLevel == MemoryGuardLevel::NORMAL && !heapHealthyForFrag) {
            newLevel = MemoryGuardLevel::WARN;
            Serial.printf("[MEMGUARD] High fragmentation (%d%%), elevating to WARN\n",
                          (int)currentHealth.heapFragmentation);
        } else if (newLevel == MemoryGuardLevel::WARN &&
                   largestBlock < guardSevereLargestThreshold &&
                   !heapHealthyForFrag) {
            // 修复M：freeHeap 健康时碎片化不再升到 SEVERE（同 largestBlock 反证）
            newLevel = MemoryGuardLevel::SEVERE;
            Serial.printf("[MEMGUARD] High fragmentation (%d%%) + small block=%lu, elevating to SEVERE\n",
                          (int)currentHealth.heapFragmentation, (unsigned long)largestBlock);
        }
    }

    // S3 的协议配置/Web 响应会在一个采样窗口内产生大块 JSON
    // 分配，实机上会把 free8 瞬时压过 SEVERE 线，但请求结束后立即恢复。
    // 只对“连续块仍安全、绝对水位未达 CRITICAL”的 S3 SEVERE 入场做
    // 两次（10s）确认。连续块跌破服务下限或任何 CRITICAL 仍立即生效。
#if CHIP_IS_ESP32S3
    const bool confirmableS3SevereEntry =
        newLevel == MemoryGuardLevel::SEVERE &&
        _lastComputedLevel < MemoryGuardLevel::SEVERE &&
        freeHeap >= criticalThreshold &&
        largestBlock >= guardSevereLargestThreshold;
    if (confirmableS3SevereEntry) {
        if (_consecutiveSeverePressureSamples < S3_SEVERE_ENTRY_CONFIRM_SAMPLES) {
            _consecutiveSeverePressureSamples++;
        }
        if (_consecutiveSeverePressureSamples < S3_SEVERE_ENTRY_CONFIRM_SAMPLES) {
            newLevel = _lastComputedLevel;
        } else {
            _consecutiveSeverePressureSamples = 0;
        }
    } else {
        _consecutiveSeverePressureSamples = 0;
    }
#else
    _consecutiveSeverePressureSamples = 0;
#endif

    // ── 迟滞防振荡：除上述 S3 安全区 SEVERE 外升级即时，
    // 降级需连续 3 次达标（时间迟滞）──
    // 避免 SSE 连接/断开等 2-5KB 抖动导致等级快速振荡。
    // 2026-08-24 修正：旧实现在降级方向额外要求 +4KB 数值迟滞带，但 .34
    // 稳态 free8≈31.7KB 永远达不到 NORMAL 阈值(30720)+4096=34816 →
    // 等级一旦升到 CRITICAL 永久无法降回，600s 后看门狗在健康堆上
    // 误触发重启（07:17:47 取证：largest=21492 frag=33% 仍被重启）。
    // 改用纯时间迟滞（连续 3 次×5s）：newLevel 由 freeHeap 计算得出，
    // 到达此处已隐含 freeHeap >= 目标阈值，防振荡靠采样次数即可。
    if (newLevel > _lastComputedLevel) {
        // 升级方向（NORMAL→WARN→SEVERE→CRITICAL）：立即生效
        _consecutiveRecoverySamples = 0;
        _lastComputedLevel = newLevel;
    } else if (newLevel < _lastComputedLevel) {
        // 降级方向：需连续 3 次达标（15s 时间迟滞）
        _consecutiveRecoverySamples++;
        if (_consecutiveRecoverySamples >= 3) {
            _lastComputedLevel = newLevel;
            _consecutiveRecoverySamples = 0;
        } else {
            newLevel = _lastComputedLevel;  // 保持当前等级
        }
    }

#if FASTBEE_ENABLE_MQTT
    // If the first SEVERE recovery could not persist protocol.json (for
    // example a transient LittleFS/write-buffer failure), the level itself
    // does not change again and applyDegradation() would never be called a
    // second time. Retry only while the effective level is still severe and
    // only when the backoff has elapsed.
    const unsigned long nowForRecovery = millis();
    if (_currentLevel >= MemoryGuardLevel::SEVERE &&
        newLevel >= MemoryGuardLevel::SEVERE &&
        !_mqttsMemoryDowngrade && !_mqttDisabledForMemory &&
        (_mqttsDowngradeRetryAtMs == 0 || nowForRecovery >= _mqttsDowngradeRetryAtMs)) {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
        MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
        bool wantsMqtts = mqtt && mqtt->getConfig().scheme == "mqtts";
#if FASTBEE_ENABLE_STORAGE_CACHE || FASTBEE_ENABLE_MQTT
        if (!mqtt) {
            JsonDocument cfgDoc;
            if (ConfigStorage::getInstance().loadProtocolSection("mqtt", cfgDoc)) {
                const char* scheme = cfgDoc["mqtt"]["scheme"] | "";
                wantsMqtts = scheme && strcmp(scheme, "mqtts") == 0;
            }
        }
#endif
        if (wantsMqtts) {
            downgradeMqttsToMqtt();
        }
    }
#endif

    // A persisted downgrade survives a reboot while _currentLevel starts at
    // NORMAL. Once a healthy sample confirms that plain MQTT can be upgraded
    // safely, run the normal recovery path even though no level transition
    // occurred during this boot. The same retry also covers a worker pool,
    // Modbus handler, or optional peripheral that was still shutting down when
    // the first NORMAL sample arrived; those states must not remain paused
    // forever merely because the level transition already happened.
    bool periphExecWorkerRecoveryPending = false;
#if FASTBEE_ENABLE_PERIPH_EXEC
    periphExecWorkerRecoveryPending =
        PeriphExecManager::getInstance().areWorkersSuspendedForMemory() ||
        PeriphExecManager::getInstance().isWorkerSuspensionPending();
#endif
    const bool pendingNormalRecovery =
        _mqttsMemoryDowngrade || _periphExecPausedForMemory ||
        periphExecWorkerRecoveryPending || _modbusRestorePendingForMemory ||
        _mqttRestorePendingForMemory ||
        _optionalPeripheralsSuspendedForMemory;
    const unsigned long recoveryNow = millis();
    const bool mqttRestoreReady =
        _mqttRestoreRetryAtMs == 0 || recoveryNow >= _mqttRestoreRetryAtMs;
    if (_currentLevel == MemoryGuardLevel::NORMAL &&
        newLevel == MemoryGuardLevel::NORMAL &&
        pendingNormalRecovery && !_mqttDisabledForMemory &&
        (_mqttsRestoreRetryAtMs == 0 || recoveryNow >= _mqttsRestoreRetryAtMs) &&
        mqttRestoreReady) {
        restoreMemoryRecovery();
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

    // G4: 主动碎片预检（NORMAL/WARN 级别下定期轻量整理，避免碎片累积到被动阈值）
    proactiveCompactCheck();

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

    // ── CRITICAL 滞留看门狗 ─────────────────────────────────────────
    // 现有碎片重启路径（severeFragmentation）要求 frag>=85% 且 largest<4KB，
    // checkCriticalMemory 重启路径要求 freeHeap<criticalThreshold。存在中间死区：
    // freeHeap 尚可但 largestBlock 卡在 4-8KB、frag<85%，设备持续 CRITICAL 降载
    // （Web 停服）却无任何重启路径触发。这里补齐：CRITICAL 持续超时则重启重组堆。
    if (_currentLevel == MemoryGuardLevel::CRITICAL) {
        if (_guardCriticalSince == 0) {
            _guardCriticalSince = millis();
        }
        unsigned long guardCriticalDuration = millis() - _guardCriticalSince;
        if (guardCriticalDuration >= GUARD_CRITICAL_STUCK_MS &&
            !SystemRebooter::isScheduled()) {
            // 重启循环保护：启动 < 120s 不重启，防止 boot loop
            if (millis() < 120000UL) {
                return;
            }
            // 重启前健康自检：若堆实际已回到安全水位（等级状态机未及降回
            // 的异常路径），不重启，强制降级并复位计时。2026-08-24
            // 07:17:47 取证：largest=21492 frag=33% heap=31748 的健康堆
            // 因降级迟滞死锁被误重启。
            if (currentHealth.dramLargestBlock >= guardCriticalLargestThreshold &&
                freeHeap >= criticalThreshold) {
                Serial.printf("[MEMGUARD] CRITICAL stuck %lus but heap recovered (largest=%lu heap=%lu), force-downgrading instead of reboot\n",
                              (unsigned long)(guardCriticalDuration / 1000),
                              (unsigned long)currentHealth.dramLargestBlock,
                              (unsigned long)freeHeap);
                MemoryGuardLevel oldLevel = _currentLevel;
                _currentLevel = MemoryGuardLevel::NORMAL;
                _lastComputedLevel = MemoryGuardLevel::NORMAL;
                _consecutiveSeverePressureSamples = 0;
                _consecutiveRecoverySamples = 0;
                _guardCriticalSince = 0;
                applyDegradation(oldLevel, MemoryGuardLevel::NORMAL);
                return;
            }
            // 重启循环抑制：上次重启已是碎片压缩重启（RTC 快照可查），且开机
            // 未超窗口又陷入 CRITICAL——说明碎片由外部探测/重试流量稳态维持，
            // 重启后会在分钟内重新碎回（.34 实测 7156B/90s），重启无用且会
            // 陷入 12 分钟无限重启环。保持降载存活，等流量间歇时自愈
            //（实测无流量 5 分钟后 blk8 18420 自恢复）。
            if (millis() < GUARD_REBOOT_LOOP_WINDOW_MS) {
                PreRestartSnapshot prevSnap;
                if (RestartDiagnostics::getLastSnapshot(prevSnap) &&
                    prevSnap.restartReason == static_cast<uint8_t>(RestartReason::MEMORY_COMPACTION)) {
                    static uint32_t lastLoopWarnMs = 0;
                    if (millis() - lastLoopWarnMs > 60000UL) {
                        lastLoopWarnMs = millis();
                        Serial.printf("[MEMGUARD] CRITICAL stuck %lus but previous reboot was also compaction; suppressing reboot to avoid loop (largest=%lu frag=%d%% heap=%lu)\n",
                                      (unsigned long)(guardCriticalDuration / 1000),
                                      (unsigned long)currentHealth.dramLargestBlock,
                                      (int)currentHealth.heapFragmentation,
                                      (unsigned long)freeHeap);
                    }
                    return;
                }
            }
            Serial.printf("[MEMGUARD] CRITICAL stuck %lus (largest=%lu frag=%d%% heap=%lu), scheduling reboot to defragment\n",
                          (unsigned long)(guardCriticalDuration / 1000),
                          (unsigned long)currentHealth.dramLargestBlock,
                          (int)currentHealth.heapFragmentation,
                          (unsigned long)freeHeap);
            char reason[48];
            snprintf(reason, sizeof(reason), "MemGuard CRITICAL stuck %lus blk=%lu",
                     (unsigned long)(guardCriticalDuration / 1000),
                     (unsigned long)currentHealth.dramLargestBlock);
            SystemRebooter::scheduleReboot(reason, 2000UL, RestartReason::MEMORY_COMPACTION);
        }
    } else {
        _guardCriticalSince = 0;  // 退出 CRITICAL，复位计时
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

    // ── SEVERE/CRITICAL 回落到 WARN：恢复 PeriphExec ─────────────────────
    // SEVERE 暂停只为缓解瞬时压力；回落到 WARN 说明压力已缓解（与调度器
    // level < SEVERE 即恢复后台任务的语义一致）。需要同时恢复 worker pool，
    // 不能只解除 scheduler 的暂停标志：worker 可能仍在异步退出或因内存不足
    // 尚未重新分配。只有 pool 确认恢复后才清除 _periphExecPausedForMemory，
    // 这样后续健康周期会继续重试，API 也不会错误地报告“已恢复”。
    if (newLevel == MemoryGuardLevel::WARN && oldLevel >= MemoryGuardLevel::SEVERE &&
        _periphExecPausedForMemory) {
        PeriphExecManager& execMgr = PeriphExecManager::getInstance();
        bool workersReady = true;
#if FASTBEE_ENABLE_PERIPH_EXEC
        if (execMgr.areWorkersSuspendedForMemory() ||
            execMgr.isWorkerSuspensionPending()) {
            workersReady = execMgr.resumeWorkersAfterMemory();
        }
#endif
        if (workersReady) {
            PeriphExecScheduler* scheduler = execMgr.getScheduler();
            if (scheduler) scheduler->setMemoryPaused(false);
            _periphExecPausedForMemory = false;
            Serial.println("[MEMRECOVER] WARN: PeriphExec workers/timers resumed (pressure eased)");
        } else {
            Serial.println("[MEMRECOVER] WARN: PeriphExec worker restore pending; keeping scheduler paused");
        }
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
        // Reclaim once on entry, and again on an actual escalation to CRITICAL.
        // An earlier stage may have retained the display after enough memory
        // was freed; a later critical condition must still be able to stop it.
        if (oldLevel < MemoryGuardLevel::SEVERE ||
            (oldLevel < MemoryGuardLevel::CRITICAL && newLevel == MemoryGuardLevel::CRITICAL)) {
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

// G4: 主动碎片预检：每 30 分钟检查一次，NORMAL/WARN 级别下碎片率 >= 50% 时
// 执行轻量整理（清日志缓存 + ConfigStorage 缓存），不触发完整 compactMemory()
void HealthMonitor::proactiveCompactCheck() {
    uint32_t now = millis();
    if (now - lastProactiveCompactTime < PROACTIVE_COMPACT_INTERVAL) return;
    lastProactiveCompactTime = now;

    // 仅在 NORMAL 或 WARN 级别下执行主动整理 (SEVERE+ 已有被动机制)
    if (_currentLevel > MemoryGuardLevel::WARN) return;

    uint8_t frag = currentHealth.heapFragmentation;
    if (frag < PROACTIVE_COMPACT_THRESHOLD) return;

    Serial.printf("[MEMGUARD] Proactive defrag: frag=%d%% (threshold=%d%%)\n",
                  frag, PROACTIVE_COMPACT_THRESHOLD);

    // 轻量整理: 仅清日志缓存 + ConfigStorage 缓存 (不关闭 SSE 连接)
    LoggerSystem::getInstance().flushBuffer();
#if FASTBEE_ENABLE_STORAGE_CACHE
    ConfigStorage::getInstance().clearCache();
#endif
    // 不触发完整的 compactMemory()，避免中断正常服务
}

void HealthMonitor::compactMemory() {
    char logBuf[128];
    snprintf(logBuf, sizeof(logBuf),
             "[MEMGUARD] High fragmentation (%d%%), performing targeted reclamation",
             (int)currentHealth.heapFragmentation);
    LOG_WARNING(logBuf);

    const size_t largestBefore = FastBee::MemoryBudget::dram8Largest();
    const uint32_t dramBefore = FastBee::MemoryBudget::dram8Free();

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

    const size_t largestAfter = FastBee::MemoryBudget::dram8Largest();
    const uint32_t dramAfter = FastBee::MemoryBudget::dram8Free();
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
    // 8BIT 口径：使用当前芯片档位的连续块临界线。
    return currentHealth.largestFreeBlock < guardCriticalLargestThreshold;
}

bool HealthMonitor::arePeriphExecWorkersSuspendedForMemory() const {
#if FASTBEE_ENABLE_PERIPH_EXEC
    return PeriphExecManager::getInstance().areWorkersSuspendedForMemory();
#else
    return false;
#endif
}

bool HealthMonitor::isPeriphExecWorkerSuspensionPending() const {
#if FASTBEE_ENABLE_PERIPH_EXEC
    return PeriphExecManager::getInstance().isWorkerSuspensionPending();
#else
    return false;
#endif
}

bool HealthMonitor::isMemoryDegraded() const {
    const bool workerSuspended = arePeriphExecWorkersSuspendedForMemory() ||
                                 isPeriphExecWorkerSuspensionPending();
    return _currentLevel >= MemoryGuardLevel::SEVERE ||
           _mqttsMemoryDowngrade || _mqttStoppedForMemory ||
           _mqttDisabledForMemory || _mqttRestorePendingForMemory ||
           _modbusStoppedForMemory ||
           _periphExecPausedForMemory || workerSuspended ||
           _optionalPeripheralsSuspendedForMemory;
}

void HealthMonitor::fillDegradationJson(JsonObject dst) const {
    static const char* levelNames[] = {"NORMAL", "WARN", "SEVERE", "CRITICAL"};
    const uint8_t level = static_cast<uint8_t>(_currentLevel);
    dst["active"] = isMemoryDegraded();
    dst["runtimeOnly"] = true;
    dst["level"] = levelNames[level < 4 ? level : 0];
    dst["mqttsDowngraded"] = _mqttsMemoryDowngrade;
    dst["mqttStopped"] = _mqttStoppedForMemory;
    dst["mqttDisabled"] = _mqttDisabledForMemory;
    dst["mqttRestorePending"] = _mqttRestorePendingForMemory;
    dst["modbusStopped"] = _modbusStoppedForMemory;
    dst["periphExecPaused"] = _periphExecPausedForMemory;
#if FASTBEE_ENABLE_PERIPH_EXEC
    dst["periphExecWorkerSuspended"] = arePeriphExecWorkersSuspendedForMemory();
    dst["periphExecWorkerSuspensionPending"] = isPeriphExecWorkerSuspensionPending();
#else
    dst["periphExecWorkerSuspended"] = false;
    dst["periphExecWorkerSuspensionPending"] = false;
#endif
    dst["optionalPeripheralsSuspended"] = _optionalPeripheralsSuspendedForMemory;
    dst["optionalPeripheralCount"] = _memorySuspendedPeripheralCount;
    JsonArray actions = dst["actions"].to<JsonArray>();
    if (_mqttsMemoryDowngrade) actions.add("MQTTS 已临时降级为 MQTT");
    if (_modbusStoppedForMemory) actions.add("Modbus 轮询已暂停");
    if (_periphExecPausedForMemory) actions.add("外设执行定时规则已暂停");
#if FASTBEE_ENABLE_PERIPH_EXEC
    if (isPeriphExecWorkerSuspensionPending()) {
        actions.add("外设执行 worker 正在退出，系统将继续重试");
    }
    if (arePeriphExecWorkersSuspendedForMemory() &&
        !isPeriphExecWorkerSuspensionPending()) {
        actions.add("外设执行 worker 已挂起，系统将继续恢复");
    }
#endif
    if (_optionalPeripheralsSuspendedForMemory) {
        actions.add("高占用可选外设已临时挂起");
    }
    if (_mqttStoppedForMemory) actions.add("MQTT 已停止");
    if (_mqttRestorePendingForMemory) actions.add("MQTT 恢复等待内存条件，系统将继续重试");
    if (_mqttDisabledForMemory) actions.add("MQTT 已禁用，需恢复内存后手动启用");
    if (actions.size() == 0 && _currentLevel == MemoryGuardLevel::WARN) {
        actions.add("已降低日志和上报频率");
    }
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

void HealthMonitor::fillMetricsJson(String& json) {
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
    doc["memguard"]["mqtt_restore_pending"] = _mqttRestorePendingForMemory;
    doc["memguard"]["modbus_stopped"]      = _modbusStoppedForMemory;
    doc["memguard"]["periph_exec_paused"]  = _periphExecPausedForMemory;
#if FASTBEE_ENABLE_PERIPH_EXEC
    doc["memguard"]["periph_exec_worker_suspended"] =
        arePeriphExecWorkersSuspendedForMemory();
    doc["memguard"]["periph_exec_worker_suspension_pending"] =
        isPeriphExecWorkerSuspensionPending();
#else
    doc["memguard"]["periph_exec_worker_suspended"] = false;
    doc["memguard"]["periph_exec_worker_suspension_pending"] = false;
#endif
    doc["memguard"]["optional_peripherals_suspended"] =
        _optionalPeripheralsSuspendedForMemory;
    doc["memguard"]["optional_peripheral_count"] = _memorySuspendedPeripheralCount;
    doc["memguard"]["critical_duration_s"] = (unsigned long)(getCriticalDurationMs() / 1000);

    // DRAM 水位趋势（泄漏检测）
    doc["stability"]["dram_history_count"] = _dramHistoryCount;
    doc["stability"]["leak_suspect"] = (_dramHistoryCount >= 6) ? detectMemoryLeak() : false;
    doc["stability"]["uptime_hours"] = (unsigned long)(currentHealth.uptime / 3600000UL);

#if FASTBEE_ENABLE_PERIPH_EXEC
    // ISR 事件队列诊断（中断风暴可观测：队列满丢事件计数 + 当前积压）
    doc["isr"]["queue_overruns"] = (unsigned long)PeripheralManager::getInstance().getIsrQueueOverrunCount();
    doc["isr"]["queue_waiting"]  = (unsigned long)PeripheralManager::getInstance().getIsrQueueWaiting();
#endif

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

    // 复用外部 String：reserve 仅扩容不缩容，已足够大时是 no-op
    json.reserve(512);
    json = "";
    serializeJson(doc, json);
}

String HealthMonitor::getMetricsJson() {
    // 兼容旧接口：内部使用复用缓冲，调用者可直接用 fillMetricsJson() 避免堆分配
    static String _buf;
    fillMetricsJson(_buf);
    return _buf;
}

// ── 定期串口输出关键指标摘要 ─────────────────────────────────────────
void HealthMonitor::logMetricsSummary() {
    // 迁移后 dram/blk 均为 8BIT 口径，不再需要单独输出 free8/total_blk
    Serial.printf("[METRICS] heap=%lu min=%lu dram8=%lu blk8=%lu frag=%d%% mqtt_q=%d sse=%d poll=%lums up=%lus\n",
        (unsigned long)currentHealth.freeHeap,
        (unsigned long)currentHealth.minFreeHeap,
        (unsigned long)currentHealth.dramFreeHeap,
        (unsigned long)currentHealth.dramLargestBlock,
        (int)currentHealth.heapFragmentation,
        (int)mqttQueueDepth,
        (int)sseClientCount,
        (unsigned long)pollDurationMs,
        (unsigned long)(currentHealth.uptime / 1000UL));
    // 输出内存池使用率（碎片预防）
    logPoolStats();
    // 周期性预算诊断：不撤销已初始化功能，运行期降级由 MemGuard/各入口负责
    FastBee::FeatureBudgetRegistry::instance().recheck();
}

size_t HealthMonitor::getTaskStackInfo(TaskStackInfo* outInfo, size_t maxTasks) const {
    const char* taskNames[] = {
        "loopTask", "async_tcp", "mqtt_reconn",
        "pexec_w0", "pexec_w1", "fb_login", "ota_url",
        "IDLE0", "IDLE1"
    };
    static constexpr size_t TASK_COUNT = sizeof(taskNames) / sizeof(taskNames[0]);
    size_t count = 0;
    for (size_t i = 0; i < TASK_COUNT && count < maxTasks; i++) {
        TaskHandle_t h = xTaskGetHandle(taskNames[i]);
        if (h) {
            strncpy(outInfo[count].name, taskNames[i], sizeof(outInfo[count].name) - 1);
            outInfo[count].name[sizeof(outInfo[count].name) - 1] = '\0';
            // ESP-IDF returns bytes, not StackType_t words.
            outInfo[count].highWaterMark = uxTaskGetStackHighWaterMark(h);
            count++;
        }
    }
    return count;
}

// ── 内存恢复：SEVERE 级别服务降级 ─────────────────────────────────────
void HealthMonitor::performMemoryRecovery(MemoryGuardLevel oldLevel, MemoryGuardLevel newLevel) {
    uint32_t dramBefore = FastBee::MemoryBudget::dram8Free();
    // 使用已有的碎片率计算函数（基于 DRAM），避免 uint8_t 截断和乘法溢出风险
    uint8_t fragPercent = (dramBefore > 0)
        ? calculateHeapFragmentationPercent(currentHealth.dramFreeHeap, currentHealth.dramLargestBlock)
        : 0;
    Serial.printf("[MEMRECOVER] === SEVERE recovery triggered === dram=%lu largest=%lu frag=%d%%\n",
                  (unsigned long)dramBefore,
                  (unsigned long)currentHealth.dramLargestBlock,
                  (int)fragPercent);

    // Keep the automatic decision visible to the feature API.  These flags
    // are runtime-only and are intentionally not written to features.json;
    // recovery can later clear them without overwriting the user's choices.
    auto& featureMgr = FastBee::RuntimeFeatureManager::instance();
    featureMgr.setRuntimeDisabled("fileLogging", "内存压力：暂时关闭文件日志");
    featureMgr.setRuntimeDisabled("modbus", "内存压力：暂停 Modbus 轮询");
    featureMgr.setRuntimeDisabled("ruleScript", "内存压力：暂停规则脚本");
    featureMgr.setRuntimeDisabled("commandScript", "内存压力：暂停命令脚本");

    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;

    // Step 0: 确保 TLS 所需连续内存（P0 优化）
    if (!currentHealth.dramLargestBlock) {
        Serial.println("[MEMRECOVER] WARN: dramLargestBlock=0, forced downgrade before TLS handshake");
    }
    ensureTLSContiguousMemory();

    // Step 1: MQTTS → MQTT 降级（释放 ~30-50KB TLS DRAM）
#if FASTBEE_ENABLE_MQTT
    if (!_mqttsMemoryDowngrade) {
        MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
        bool wantsMqtts = mqtt && mqtt->getConfig().scheme == "mqtts";
#if FASTBEE_ENABLE_STORAGE_CACHE || FASTBEE_ENABLE_MQTT
        if (!mqtt) {
            JsonDocument cfgDoc;
            if (ConfigStorage::getInstance().loadProtocolSection("mqtt", cfgDoc)) {
                const char* scheme = cfgDoc["mqtt"]["scheme"] | "";
                wantsMqtts = scheme && strcmp(scheme, "mqtts") == 0;
            }
        }
#endif
        if (wantsMqtts) {
            // Only mark MQTTS as runtime-disabled when this device actually
            // uses the TLS transport (or is carrying a persisted downgrade).
            // Plain-MQTT devices must not show a permanent, unrelated MQTTS
            // warning after a transient severe-memory event.
            featureMgr.setRuntimeDisabled("mqtts", "内存压力：TLS 暂时降级为 MQTT");
            Serial.println("[MEMRECOVER] Step 1/3: MQTTS -> MQTT downgrade (releasing ~35KB TLS DRAM)");
            downgradeMqttsToMqtt();
        }
        else if (!_mqttsMemoryDowngrade) {
            featureMgr.clearRuntimeDisabled("mqtts");
        }
    }
#endif

    // Step 2: 停止 Modbus RTU（释放串口缓冲 + 停止轮询 CPU 开销）
#if FASTBEE_ENABLE_MODBUS
    if (pm && !_modbusStoppedForMemory) {
        auto modbusLease = pm->acquireModbusHandler(pdMS_TO_TICKS(100));
        if (modbusLease && modbusLease->isRunning()) {
            Serial.println("[MEMRECOVER] Step 2/3: Modbus stopped (was running)");
            pm->stopModbus();
            _modbusStoppedForMemory = true;
        }
    }
#endif

    // Step 3: 暂停 PeriphExec 定时器（减少 CPU 和内存峰值）。即使定时器
    // 已经暂停，也要继续尝试关闭仍在执行的 worker；第一次等待超时不能
    // 把“尚未释放”误记成完成，否则后续健康采样不会再重试。
    {
        PeriphExecManager& execMgr = PeriphExecManager::getInstance();
        PeriphExecScheduler* scheduler = execMgr.getScheduler();
        if (!_periphExecPausedForMemory && scheduler) {
            Serial.println("[MEMRECOVER] Step 3/3: PeriphExec timers paused");
            scheduler->setMemoryPaused(true);
            _periphExecPausedForMemory = true;
        }
#if FASTBEE_ENABLE_PERIPH_EXEC
        if (!execMgr.areWorkersSuspendedForMemory()) {
            // Keep the degraded flag set while a worker is still draining so
            // later health cycles can retry and the restore path can wait for
            // a fully stopped pool.
            _periphExecPausedForMemory = true;
            // Pausing timers alone leaves the resident worker stacks allocated.
            // Retry until every worker acknowledges exit.
            if (!execMgr.suspendWorkersForMemory()) {
                Serial.println("[MEMRECOVER] PeriphExec worker shutdown pending; will retry");
            }
        }
#endif
    }

#if FASTBEE_ENABLE_PERIPH_EXEC
    // Modbus/worker teardown may already have removed the pressure. Sample
    // live internal DRAM here, not currentHealth (the pre-recovery snapshot),
    // before disrupting a visible display or another optional peripheral.
    // Use this target's actual SEVERE bands, including contiguous capacity.
    // This only skips the next teardown stage: paused services and guard
    // hysteresis stay unchanged and recover through the normal stable-sample
    // path. A later CRITICAL escalation retries the stage immediately.
    if (!_optionalPeripheralsSuspendedForMemory) {
        const uint32_t remainingDram = FastBee::MemoryBudget::dram8Free();
        const uint32_t remainingLargest = FastBee::MemoryBudget::dram8Largest();
        if (remainingDram < guardSevereDramThreshold ||
            remainingLargest < guardSevereLargestThreshold) {
            size_t suspended = PeripheralManager::getInstance().suspendOptionalForMemory();
            _memorySuspendedPeripheralCount = static_cast<uint16_t>(
                suspended > UINT16_MAX ? UINT16_MAX : suspended);
            _optionalPeripheralsSuspendedForMemory = suspended > 0;
            if (suspended > 0) {
                Serial.printf("[MEMRECOVER] Optional peripherals suspended: %u\n",
                              (unsigned)_memorySuspendedPeripheralCount);
            }
        } else {
            Serial.printf("[MEMRECOVER] Optional peripherals retained: dram=%lu largest=%lu\n",
                          (unsigned long)remainingDram, (unsigned long)remainingLargest);
        }
    }
#endif

    uint32_t dramAfter = FastBee::MemoryBudget::dram8Free();
    Serial.printf("[MEMRECOVER] === SEVERE recovery complete === dram=%lu (gained %ld bytes)\n",
                  (unsigned long)dramAfter, (long)(dramAfter - dramBefore));
}

// ── MQTTS → MQTT 降级：写入配置文件（含 downgraded_by_memory 标记）并重启 MQTT ──
// 采用保守方案：持久化 + downgraded_by_memory 标记，restoreMemoryRecovery() 自动回写
void HealthMonitor::downgradeMqttsToMqtt() {
    const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE;

    // Validate the section before writing.  updateJsonConfig() intentionally
    // accepts a missing document for general callers; using it blindly here
    // could turn a transient read failure into an empty protocol.json while
    // the device is already under memory pressure.
#if FASTBEE_ENABLE_STORAGE_CACHE || FASTBEE_ENABLE_MQTT
    JsonDocument currentDoc;
    if (!ConfigStorage::getInstance().loadProtocolSection("mqtt", currentDoc) ||
        !currentDoc["mqtt"].is<JsonObject>()) {
        _mqttsDowngradeRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
        Serial.println("[MEMRECOVER]   WARN: MQTT config section unavailable, retry downgrade later");
        return;
    }
#else
    _mqttsDowngradeRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
    Serial.println("[MEMRECOVER]   WARN: ConfigStorage unavailable, retry downgrade later");
    return;
#endif

    // 只在第一次降级时抓取原始值，避免连续压力事件覆盖恢复信息。
    if (!_mqttsMemoryDowngrade) {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
        MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
        if (mqtt) {
            const MQTTConfig& cfg = mqtt->getConfig();
            strncpy(_mqttsOriginalScheme, cfg.scheme.c_str(), sizeof(_mqttsOriginalScheme) - 1);
            _mqttsOriginalScheme[sizeof(_mqttsOriginalScheme) - 1] = '\0';
            _mqttsOriginalPort = cfg.port > 0 ? cfg.port : 8883;
        } else {
            String scheme = currentDoc["mqtt"]["scheme"] | "mqtts";
            if (scheme.isEmpty()) scheme = "mqtts";
            strncpy(_mqttsOriginalScheme, scheme.c_str(), sizeof(_mqttsOriginalScheme) - 1);
            _mqttsOriginalScheme[sizeof(_mqttsOriginalScheme) - 1] = '\0';
            const uint32_t port = currentDoc["mqtt"]["port"] | 8883U;
            _mqttsOriginalPort = (port > 0 && port <= 65535U)
                ? static_cast<uint16_t>(port) : 8883;
        }
    }
    Serial.println("[MEMRECOVER]   protocol.json updating: scheme=mqtt, port=1883, downgraded_by_memory=true");

    // 写入配置文件：scheme=mqtt, port=1883, downgraded_by_memory=true
    bool configUpdated = HandlerUtils::updateJsonConfig(configPath,
        [this](JsonDocument& doc) {
            if (doc["mqtt"].is<JsonObject>()) {
                doc["mqtt"]["scheme"] = "mqtt";
                doc["mqtt"]["port"] = 1883;
                doc["mqtt"]["downgraded_by_memory"] = true;
                doc["mqtt"]["downgraded_original_scheme"] = _mqttsOriginalScheme;
                doc["mqtt"]["downgraded_original_port"] = _mqttsOriginalPort;
            }
        });

    if (!configUpdated) {
        _mqttsDowngradeRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
        Serial.println("[MEMRECOVER]   WARN: failed to update protocol.json, skip MQTT restart");
        return;  // 配置写入失败，不标记已降级，不重启 MQTT
    }

    // Confirm the persisted values before changing in-memory state.  This
    // keeps the status API truthful even if LittleFS accepted the open/write
    // but a later flush/rename left stale content on disk.
    JsonDocument verifyDoc;
    const bool verified = ConfigStorage::getInstance().loadProtocolSection("mqtt", verifyDoc) &&
        verifyDoc["mqtt"].is<JsonObject>() &&
        strcmp(verifyDoc["mqtt"]["scheme"] | "", "mqtt") == 0 &&
        (verifyDoc["mqtt"]["port"] | 0U) == 1883U &&
        (verifyDoc["mqtt"]["downgraded_by_memory"] | false);
    if (!verified) {
        _mqttsDowngradeRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
        Serial.println("[MEMRECOVER]   WARN: protocol.json verification failed, skip MQTT restart");
        return;
    }
    _mqttsDowngradeRetryAtMs = 0;
    Serial.println("[MEMRECOVER]   protocol.json updated: scheme=mqtt, port=1883, downgraded_by_memory=true");

    // 标记已降级（防止重复操作）+ 记录降级时间
    _mqttsMemoryDowngrade = true;
    _mqttsDowngradeTimeMs = millis();

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
    uint32_t dramAfter = FastBee::MemoryBudget::dram8Free();
    Serial.printf("[MEMRECOVER]   DRAM after downgrade: %lu bytes\n", (unsigned long)dramAfter);
}

// Confirm deferred services after loopTask has had a chance to rebuild them.
// The Web/API status must reflect the actual handler state, not merely that a
// restart flag was accepted by ProtocolManager.
void HealthMonitor::reconcilePendingMemoryRecovery() {
#if FASTBEE_ENABLE_PERIPH_EXEC
    // A worker can still be draining a long-running rule when the first
    // SEVERE sample arrives. Keep retrying the safe shutdown on every health
    // cycle while pressure remains severe; waiting for a level transition
    // would leave the resident worker stacks allocated indefinitely.
    //
    // Conversely, a previous implementation cleared _periphExecPausedForMemory
    // as soon as the level fell to WARN and resumed only the scheduler. If the
    // worker pool was still suspended (or its replacement allocation failed),
    // the API reported NORMAL while rules silently stopped executing. Treat the
    // worker state itself as pending and retry restoration at WARN/NORMAL too.
    PeriphExecManager& execMgr = PeriphExecManager::getInstance();
    const bool workersSuspended = execMgr.areWorkersSuspendedForMemory() ||
                                  execMgr.isWorkerSuspensionPending();
    if (_currentLevel >= MemoryGuardLevel::SEVERE) {
        if (_periphExecPausedForMemory && !execMgr.areWorkersSuspendedForMemory()) {
            if (execMgr.suspendWorkersForMemory()) {
                Serial.println("[MEMRECOVER]   PeriphExec workers fully suspended after retry");
            } else {
                Serial.println("[MEMRECOVER]   PeriphExec worker shutdown still pending");
            }
        }
    } else if (workersSuspended || _periphExecPausedForMemory) {
        // Keep the flag set until both the pool and scheduler are usable.
        _periphExecPausedForMemory = true;
        if (execMgr.resumeWorkersAfterMemory()) {
            PeriphExecScheduler* scheduler = execMgr.getScheduler();
            if (scheduler) scheduler->setMemoryPaused(false);
            _periphExecPausedForMemory = false;
            Serial.println("[MEMRECOVER]   PeriphExec workers/timers restored after retry");
        } else {
            Serial.println("[MEMRECOVER]   PeriphExec worker restore still pending");
        }
    }
#endif

#if FASTBEE_ENABLE_MQTT
    // MQTT recovery is intentionally confirmed separately from the request
    // that queued it.  restartMQTTDeferred() can be rejected by the heap
    // admission guard; clearing the memory-stop flag in that case would make
    // REST/SSE claim recovery while no client exists.
    if (_mqttRestorePendingForMemory && !_mqttDisabledForMemory &&
        _currentLevel < MemoryGuardLevel::SEVERE) {
        const unsigned long now = millis();
        if (_mqttRestoreRetryAtMs == 0 || now >= _mqttRestoreRetryAtMs) {
            FastBeeFramework* fw = FastBeeFramework::getInstance();
            ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
            MQTTClient* mqtt = pm ? pm->getMQTTClient() : nullptr;
            if (mqtt && !mqtt->isStopped() && !mqtt->isStoppedByMemory()) {
                _mqttRestorePendingForMemory = false;
                _mqttRestoreRetryAtMs = 0;
                _mqttStoppedForMemory = false;
                mqtt->setStoppedByMemory(false);
                Serial.println("[MEMRECOVER]   MQTT handler already usable; cleared degraded state");
            } else {
                const bool restartQueued = pm && pm->restartMQTTDeferred();
                MQTTClient* restored = pm ? pm->getMQTTClient() : nullptr;
                if (restartQueued && restored && !restored->isStopped()) {
                    _mqttRestorePendingForMemory = false;
                    _mqttRestoreRetryAtMs = 0;
                    _mqttStoppedForMemory = false;
                    restored->setStoppedByMemory(false);
                    Serial.println("[MEMRECOVER]   MQTT restart queued and client ready; cleared degraded state");
                } else {
                    _mqttRestoreRetryAtMs = now + MQTT_MEMORY_CONFIG_RETRY_MS;
                    Serial.println("[MEMRECOVER]   WARN: MQTT deferred restart rejected or client not ready; keeping degraded state");
                }
            }
        }
    }
#endif

#if FASTBEE_ENABLE_MODBUS
    if (_modbusRestorePendingForMemory) {
        FastBeeFramework* fw = FastBeeFramework::getInstance();
        ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
        if (!pm) return;

        {
            auto modbusLease = pm->acquireModbusHandler(pdMS_TO_TICKS(100));
            if (modbusLease && modbusLease->isRunning()) {
                _modbusRestorePendingForMemory = false;
                _modbusRestoreRetryAtMs = 0;
                _modbusStoppedForMemory = false;
                FastBee::RuntimeFeatureManager::instance().clearRuntimeDisabled("modbus");
                Serial.println("[MEMRECOVER]   Modbus handler confirmed running; cleared degraded state");
                return;
            }
        }

        // The protocol loop has not consumed the request yet. Avoid stacking
        // duplicate flags while it is pending.
        if (pm->isModbusRestartPending()) return;

        ModbusRestartResult restartResult = ModbusRestartResult::Failed;
        if (pm->consumeModbusRestartResult(restartResult) &&
            restartResult == ModbusRestartResult::IdleNoEnabledTargets) {
            // The user may disable every template while Modbus is paused for
            // memory recovery. That is a valid steady state: no handler is
            // expected, and retrying every 30 seconds would only churn JSON
            // allocations and misleading warnings.
            _modbusRestorePendingForMemory = false;
            _modbusRestoreRetryAtMs = 0;
            _modbusStoppedForMemory = false;
            FastBee::RuntimeFeatureManager::instance().clearRuntimeDisabled("modbus");
            Serial.println("[MEMRECOVER]   Modbus has no enabled targets; cleared degraded state in idle mode");
            return;
        }

        const unsigned long now = millis();
        if (_modbusRestoreRetryAtMs != 0 && now < _modbusRestoreRetryAtMs) return;

        if (pm->restartModbusDeferred()) {
            _modbusRestoreRetryAtMs = now + MODBUS_MEMORY_RETRY_MS;
            Serial.println("[MEMRECOVER]   Modbus handler still down; queued another deferred restart");
        } else {
            _modbusRestoreRetryAtMs = now + MODBUS_MEMORY_RETRY_MS;
            Serial.println("[MEMRECOVER]   WARN: Modbus deferred restart rejected; keeping degraded state");
        }
    }
#endif
}

// ── NORMAL 恢复：重启因内存停止的服务 ────────────────────────────────
void HealthMonitor::restoreMemoryRecovery() {
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;

    Serial.println("[MEMRECOVER] === NORMAL: restoring services ===");

    auto& featureMgr = FastBee::RuntimeFeatureManager::instance();
    featureMgr.clearRuntimeDisabled("fileLogging");
    featureMgr.clearRuntimeDisabled("ruleScript");
    featureMgr.clearRuntimeDisabled("commandScript");
    if (!_mqttsMemoryDowngrade) {
        // A severe-memory episode on a plain-MQTT device must not leave a
        // stale MQTTS runtime warning in the feature status API.
        featureMgr.clearRuntimeDisabled("mqtts");
    }
    // 必须在排队延迟重启前清除本轮内存保护留下的运行时禁用标志。
    // 否则 restartModbus() 会把恢复请求再次拦截为 disabled，pending
    // 状态会一直保留，即使堆已经回到 NORMAL。
    featureMgr.clearRuntimeDisabled("modbus");

    // 恢复 Modbus（如果被内存停止过）
#if FASTBEE_ENABLE_MODBUS
    // Queue the first recovery once. Repeated NORMAL health samples also call
    // this function before reconcilePendingMemoryRecovery(); requeueing here
    // hides a running handler behind restartPending, discards an idle result,
    // and continuously postpones the failure retry deadline.
    if (_modbusStoppedForMemory && !_modbusRestorePendingForMemory) {
        if (pm && pm->restartModbusDeferred()) {
            // restartModbusDeferred() only queues work for loopTask. Keep the
            // degraded flag until a later health sample confirms that the
            // handler was actually rebuilt and started.
            _modbusRestorePendingForMemory = true;
            _modbusRestoreRetryAtMs = millis() + MODBUS_MEMORY_RETRY_MS;
            Serial.println("[MEMRECOVER]   Modbus restart queued; waiting for handler confirmation");
        } else {
            _modbusRestorePendingForMemory = true;
            _modbusRestoreRetryAtMs = millis() + MODBUS_MEMORY_RETRY_MS;
            Serial.println("[MEMRECOVER]   WARN: Modbus restart could not be queued; keeping degraded state");
        }
    }
#endif

    // 恢复 PeriphExec 定时器
    if (_periphExecPausedForMemory) {
        PeriphExecManager& execMgr = PeriphExecManager::getInstance();
        bool workersReady = true;
#if FASTBEE_ENABLE_PERIPH_EXEC
        workersReady = execMgr.resumeWorkersAfterMemory();
#endif
        if (workersReady) {
            PeriphExecScheduler* scheduler = execMgr.getScheduler();
            if (scheduler) {
                scheduler->setMemoryPaused(false);
            }
            _periphExecPausedForMemory = false;
            Serial.println("[MEMRECOVER]   PeriphExec workers/timers resumed");
        } else {
            Serial.println("[MEMRECOVER]   PeriphExec worker restore deferred; timers remain paused");
        }
    }

#if FASTBEE_ENABLE_PERIPH_EXEC
    if (_optionalPeripheralsSuspendedForMemory) {
        size_t restored = PeripheralManager::getInstance().restoreOptionalAfterMemory();
        _memorySuspendedPeripheralCount = static_cast<uint16_t>(
            PeripheralManager::getInstance().getMemorySuspendedCount() > UINT16_MAX
                ? UINT16_MAX
                : PeripheralManager::getInstance().getMemorySuspendedCount());
        if (_memorySuspendedPeripheralCount == 0) {
            _optionalPeripheralsSuspendedForMemory = false;
        }
        if (restored > 0) {
            Serial.printf("[MEMRECOVER]   Optional peripherals restored: %u\n",
                          (unsigned)restored);
        }
    }
#endif

    // 恢复 MQTT：被禁用时不自动恢复，用户需手动在 Web 界面开启
#if FASTBEE_ENABLE_MQTT
    if (_mqttDisabledForMemory) {
        Serial.println("[MEMRECOVER]   MQTT stays DISABLED (config: enabled=false), user must re-enable manually");
        // 不重置 _mqttDisabledForMemory，不重启 MQTT
        _mqttRestorePendingForMemory = false;
        _mqttRestoreRetryAtMs = 0;
    } else if (_mqttStoppedForMemory &&
               (_mqttRestoreRetryAtMs == 0 || millis() >= _mqttRestoreRetryAtMs)) {
        const bool restartQueued = pm && pm->restartMQTTDeferred();
        MQTTClient* restored = pm ? pm->getMQTTClient() : nullptr;
        if (restartQueued && restored && !restored->isStopped()) {
            _mqttRestorePendingForMemory = false;
            _mqttRestoreRetryAtMs = 0;
            _mqttStoppedForMemory = false;
            restored->setStoppedByMemory(false);
            Serial.println("[MEMRECOVER]   MQTT restart queued and client ready; cleared degraded state");
        } else {
            // Keep the memory-stop marker visible until a later health cycle
            // can queue a successful rebuild.  Clearing it here used to make
            // REST/SSE report recovery while MQTT remained absent.
            _mqttRestorePendingForMemory = true;
            _mqttRestoreRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
            Serial.println(restartQueued
                ? "[MEMRECOVER]   WARN: MQTT restart queued but client not ready; keeping degraded state"
                : "[MEMRECOVER]   WARN: MQTT restart could not be queued; keeping degraded state");
        }
    }
#endif

    // 恢复 MQTTS：降级后自动回写配置并重启，带 24h 冷却防风暴
    // 若 MQTTS 握手再次导致内存压力，MemGuard 会重新降级，
    // 24h 冷却确保不会在 NORMAL 与 SEVERE 之间振荡。
#if FASTBEE_ENABLE_MQTT
    // When the critical-path guard had to disable MQTT completely, keep the
    // user-visible disabled decision authoritative.  Do not silently restore
    // an encrypted transport while MQTT is still disabled; the original
    // scheme/port marker remains on disk for an explicit user re-enable.
    if (_mqttsMemoryDowngrade && !_mqttDisabledForMemory) {
        if (_mqttsRestoreCooldownMs > 0 && millis() < _mqttsRestoreCooldownMs) {
            unsigned long remainingMs = _mqttsRestoreCooldownMs - millis();
            Serial.printf("[MEMRECOVER]   MQTTS restore on cooldown (%lus remaining), keeping plain MQTT\n",
                          (unsigned long)(remainingMs / 1000));
        } else {
            Serial.println("[MEMRECOVER]   Restoring MQTTS (memory recovered, writing back config)");
            const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE;
            const String originalScheme = String(_mqttsOriginalScheme[0] ? _mqttsOriginalScheme : "mqtts");
            const uint16_t originalPort = _mqttsOriginalPort > 0 ? _mqttsOriginalPort : 8883;
#if FASTBEE_ENABLE_STORAGE_CACHE || FASTBEE_ENABLE_MQTT
            JsonDocument currentDoc;
            const bool sectionAvailable =
                ConfigStorage::getInstance().loadProtocolSection("mqtt", currentDoc) &&
                currentDoc["mqtt"].is<JsonObject>();
#else
            const bool sectionAvailable = false;
#endif
            bool configRestored = false;
            if (sectionAvailable) {
                const bool writeOk = HandlerUtils::updateJsonConfig(configPath,
                    [originalScheme, originalPort](JsonDocument& doc) {
                        if (doc["mqtt"].is<JsonObject>()) {
                            doc["mqtt"]["scheme"] = originalScheme;
                            doc["mqtt"]["port"] = originalPort;
                            doc["mqtt"]["downgraded_by_memory"] = false;
                            // Keep the original values for diagnostics and a
                            // future retry after another reboot.
                            doc["mqtt"]["downgraded_original_scheme"] = originalScheme;
                            doc["mqtt"]["downgraded_original_port"] = originalPort;
                        }
                    });
                if (writeOk) {
                    JsonDocument verifyDoc;
                    configRestored = ConfigStorage::getInstance().loadProtocolSection("mqtt", verifyDoc) &&
                        verifyDoc["mqtt"].is<JsonObject>() &&
                        strcmp(verifyDoc["mqtt"]["scheme"] | "", originalScheme.c_str()) == 0 &&
                        (verifyDoc["mqtt"]["port"] | 0U) == originalPort &&
                        !(verifyDoc["mqtt"]["downgraded_by_memory"] | false);
                }
            }

            if (!configRestored) {
                _mqttsRestoreRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
                Serial.println("[MEMRECOVER]   WARN: failed to verify MQTTS restore, keeping plain MQTT");
            } else {
                // Writing the original scheme is only half of the recovery:
                // the TLS client must also be rebuilt successfully.  A
                // deferred rebuild can be rejected by the heap admission
                // guard, so do not clear the persisted/runtime downgrade
                // state until a usable client is confirmed.
                bool restartQueued = false;
                MQTTClient* restored = nullptr;
                if (pm) {
                    pm->stopMQTT();
                    restartQueued = pm->restartMQTTDeferred(true);  // forceRebuild=true，新配置生效
                    restored = pm->getMQTTClient();
                }
                const bool clientReady = restartQueued && restored && !restored->isStopped();
                if (clientReady) {
                    _mqttsRestoreRetryAtMs = 0;
                    _mqttsMemoryDowngrade = false;
                    _mqttsRestoreCooldownMs = millis() + 24UL * 60 * 60 * 1000;  // 24h 冷却
                    featureMgr.clearRuntimeDisabled("mqtts");
                    Serial.printf("[MEMRECOVER]   MQTTS config restored and client ready (scheme=%s, port=%u)\n",
                                  _mqttsOriginalScheme, (unsigned)_mqttsOriginalPort);
                } else {
                    // Keep the device on plain MQTT if TLS could not be
                    // rebuilt.  This also persists the marker again so a
                    // reboot cannot immediately retry a memory-expensive TLS
                    // handshake before HealthMonitor has started.
                    _mqttsMemoryDowngrade = true;
                    featureMgr.setRuntimeDisabled("mqtts", "内存恢复后等待 TLS 客户端重建");
                    downgradeMqttsToMqtt();
                    _mqttsRestoreRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
                    Serial.println(restartQueued
                        ? "[MEMRECOVER]   WARN: MQTTS client not ready after deferred restart; reverted to plain MQTT"
                        : "[MEMRECOVER]   WARN: MQTTS deferred restart rejected; reverted to plain MQTT");
                }
            }
        }
    }
#endif

    // 重置 CRITICAL 计时
    _criticalStartTime = 0;

    const bool restorePending = _periphExecPausedForMemory ||
                                _modbusRestorePendingForMemory ||
                                _mqttRestorePendingForMemory ||
                                _optionalPeripheralsSuspendedForMemory ||
                                (_mqttsMemoryDowngrade && !_mqttDisabledForMemory);
    Serial.println(restorePending
        ? "[MEMRECOVER] === NORMAL restore pending (will retry) ==="
        : "[MEMRECOVER] === NORMAL restore complete ===");
}

// ── TLS 预留池强制检查（P0 优化）───────────────────────────────────────
void HealthMonitor::ensureTLSContiguousMemory() {
#if FASTBEE_ENABLE_MQTT
    // 仅对 MQTTS 启用机型进行检查
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
    if (!pm) return;

    MQTTClient* mqtt = pm->getMQTTClient();
    if (!mqtt || mqtt->getConfig().scheme != "mqtts") return;

    uint32_t largestBlock = currentHealth.dramLargestBlock;
    const uint32_t tlsRequired = 32768U;  // 32KB 连续 DRAM（见 ChipConfig.h:TLS_RESERVED_BLOCK_BYTES）

    // 检查最大连续块是否满足 TLS 握手需求
    if (largestBlock >= tlsRequired) return;  // 已满足，直接返回

    Serial.printf("[TLS-RESERVE] WARN: Largest contiguous DRAM=%u < %u required for TLS handshake\n",
                  largestBlock, tlsRequired);

    // 再次触发内存回收（应该已经执行过了，但作为兜底）
    if (largestBlock < 16384U) {  // 小于 16KB，立即降级到非 TLS
        Serial.println("[TLS-RESERVE] CRITICAL: Insufficient contiguous memory (<16KB), forcing downgrade");

        if (!_mqttsMemoryDowngrade) {
            downgradeMqttsToMqtt();
            Serial.println("[TLS-RESERVE] Forced downgrade to plain MQTT due to TLS memory pressure");
        }
    }
#endif
}

// ── 永久禁用 MQTT：写入配置 + 停止（CRITICAL 30s 最后手段）────────────
void HealthMonitor::disableMqttForMemory() {
    Serial.println("[MEMRECOVER]   Disabling MQTT in config (memory critical)");
    const char* configPath = FileSystem::PROTOCOL_CONFIG_FILE;
#if FASTBEE_ENABLE_STORAGE_CACHE || FASTBEE_ENABLE_MQTT
    JsonDocument currentDoc;
    const bool sectionAvailable =
        ConfigStorage::getInstance().loadProtocolSection("mqtt", currentDoc) &&
        currentDoc["mqtt"].is<JsonObject>();
#else
    const bool sectionAvailable = false;
#endif
    if (!sectionAvailable) {
        _mqttDisabledForMemory = false;
        _mqttStoppedForMemory = false;
        _mqttDisableRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
        Serial.println("[MEMRECOVER]   WARN: MQTT config section unavailable, will retry disable");
        return;
    }
    bool ok = HandlerUtils::updateJsonConfig(configPath,
        [](JsonDocument& doc) {
            if (doc["mqtt"].is<JsonObject>()) {
                doc["mqtt"]["enabled"] = false;
                doc["mqtt"]["disabled_by_memory"] = true;
                // Remove the compatibility alias if a previous firmware wrote
                // it, leaving one authoritative marker on disk.
                doc["mqtt"].remove("disable_mqtt_by_memory");
            }
        });
    JsonDocument verifyDoc;
    const bool verified = ok &&
        ConfigStorage::getInstance().loadProtocolSection("mqtt", verifyDoc) &&
        verifyDoc["mqtt"].is<JsonObject>() &&
        !(verifyDoc["mqtt"]["enabled"] | true) &&
        (verifyDoc["mqtt"]["disabled_by_memory"] | false);
    if (!verified) {
        _mqttDisabledForMemory = false;
        _mqttStoppedForMemory = false;
        _mqttDisableRetryAtMs = millis() + MQTT_MEMORY_CONFIG_RETRY_MS;
        Serial.println("[MEMRECOVER]   WARN: failed to disable MQTT in config");
        return;
    }
    _mqttDisableRetryAtMs = 0;
    Serial.println("[MEMRECOVER]   protocol.json updated: mqtt.enabled=false, disabled_by_memory=true");

    // 立即停止 MQTT（配置已更新，重启后也不会重连）
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    ProtocolManager* pm = fw ? fw->getProtocolManager() : nullptr;
    if (pm) {
        if (MQTTClient* mqtt = pm->getMQTTClient()) {
            // Set the marker before stopMQTT() emits its final SSE event.
            mqtt->setDisabledByMemory(true);
            mqtt->setStoppedByMemory(true);
        }
        pm->stopMQTT();
        pm->markMqttDisabledByConfig();
        Serial.println("[MEMRECOVER]   MQTT stopped (will NOT reconnect after reboot)");
    }
}
