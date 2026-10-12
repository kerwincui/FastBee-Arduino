#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "utils/HeapFragmentation.h"
#include "core/MemoryBudget.h"
#include "core/interfaces/ILoggerSystem.h"  // LogLevel 枚举

// 内存保护等级。CRITICAL 空闲 DRAM 下限按运行时 PSRAM 档位动态计算，
// 避免无 PSRAM 设备的稳定低内存基线被误判为不可恢复故障。
enum class MemoryGuardLevel : uint8_t {
    NORMAL = 0,
    WARN = 1,
    SEVERE = 2,
    CRITICAL = 3
};

// 阈值常量（贴合 ESP32 no-PSRAM 实际：空闲 heap~27KB，web服务时临时降至 10-15KB 是正常行为）
static constexpr uint32_t MEM_THRESHOLD_NORMAL  = FastBee::MemoryBudget::GUARD_WARN_DRAM_FREE;
static constexpr uint32_t MEM_THRESHOLD_WARN    = FastBee::MemoryBudget::GUARD_SEVERE_DRAM_FREE;
static constexpr uint32_t MEM_THRESHOLD_SEVERE  = FastBee::MemoryBudget::GUARD_CRITICAL_DRAM_FREE;
// largestFreeBlock 健康反证：连续块 >= 12KB 足以响应 HTTP 响应，强制维持 NORMAL
static constexpr uint32_t MEM_LARGEST_HEALTHY    = FastBee::MemoryBudget::GUARD_WARN_LARGEST_BLOCK;
static constexpr uint8_t  FRAG_THRESHOLD_COMPACT = 80;    // 碎片率80%触发紧凑化
static constexpr uint8_t  FRAG_THRESHOLD_REBOOT  = 85;    // sustained severe fragmentation triggers reboot
static constexpr uint32_t FRAG_REBOOT_MAX_BLOCK  = 4096;  // 4KB largest DRAM block is too small for stable TCP/Web
static constexpr uint32_t FRAG_REBOOT_COUNT      = 12;    // 12 * 5s health checks = 60s

// 任务栈水位信息
struct TaskStackInfo {
    char name[16];
    uint32_t highWaterMark = UINT32_MAX; // bytes; valid only when observed
    bool observed = false;
    uint64_t sampledAtUptimeMs = 0;
};

// WiFi 连接内存保护阈值（DRAM，排除 PSRAM）
// WiFi.begin() 触发驱动初始化、lwIP TCP/IP 栈等需要约 12-16KB DRAM
static constexpr uint32_t WIFI_CONNECT_MIN_DRAM  = 16384;  // 16KB — connectToWiFi() 入口检查
static constexpr uint32_t WIFI_RECONN_MIN_DRAM   = 12288;  // 12KB — attemptReconnect() 轻量检查

// 系统健康状态快照
struct SystemHealth {
    uint32_t     freeHeap;           // 当前空闲堆（字节，含 PSRAM）
    uint32_t     minFreeHeap;        // 历史最小空闲堆（字节，含 PSRAM）
    uint8_t      heapFragmentation;  // 堆碎片率（0-100%，基于 DRAM）
    bool         fileSystemOK;       // 文件系统是否正常
    bool         wifiConnected;      // WiFi 是否已连接
    int8_t       wifiStrength;       // WiFi RSSI（dBm）
    unsigned long uptime;            // 系统运行时间（毫秒）
    uint8_t      cpuUsage;           // CPU 估算占用（0-100%）
    uint32_t     largestFreeBlock;   // 最大连续可用块（字节，DRAM 内部）
    unsigned long bootTimeMs;        // 启动耗时（毫秒）
    // ── DRAM 专项字段（排除 PSRAM，用于 WiFi/MQTT/SSL 内存保护决策）──
    uint32_t     dramFreeHeap;       // DRAM 内部空闲（MALLOC_CAP_INTERNAL）
    uint32_t     dramLargestBlock;   // DRAM 内部最大连续块（MALLOC_CAP_INTERNAL）
    uint8_t      fragmentationSeverity;  // 碎片率严重度 (0-3, Phase A Task A.5)
};

class HealthMonitor {
public:
    HealthMonitor();
    bool initialize();
    void update();
    void shutdown();

    SystemHealth getHealthStatus() const { return currentHealth; }

    // 将健康报告写入 char 缓冲区（零 String 对象），返回写入字节数
    size_t getHealthReport(char* buf, size_t bufSize) const;

    // 兼容旧接口：返回 String（内部调用 getHealthReport()）
    String getHealthReport();

    bool isSystemHealthy() const;

    // Only the calling task; name is the scope label "current-task", not an RTOS name.
    size_t getTaskStackInfo(TaskStackInfo* outInfo, size_t maxTasks) const;

    // ── 内存保护等级 ──
    MemoryGuardLevel getMemoryGuardLevel() const;
    bool isMemoryNormal() const;
    bool isMemoryWarning() const;
    bool isMemorySevere() const;
    bool isMemoryCritical() const;

    // 运行期降级状态（供 Web/API 及诊断页面统一展示）。这些状态只反映
    // 当前运行时保护动作，不会把用户的外设/规则配置静默写成 disabled。
    bool isMemoryDegraded() const;
    void fillDegradationJson(JsonObject dst) const;
    bool isFragmentationHigh() const;  // 碎片率>75%

    // ── 内存恢复状态查询（供 API 和外部模块使用）──
    bool isMqttsDowngraded() const { return false; } // Legacy API: no automatic plaintext downgrade.
    bool isMqttsPausedForMemory() const { return _mqttsMemoryDowngrade; }
    bool isMqttStoppedForMemory() const { return _mqttStoppedForMemory; }
    bool isMqttDisabledForMemory() const { return _mqttDisabledForMemory; }
    bool isMqttRestorePendingForMemory() const { return _mqttRestorePendingForMemory; }
    // 用户显式保存 MQTT 配置后，清除此前由内存保护留下的自动覆盖状态。
    // 该状态与 protocol.json 中的 disabled_by_memory/downgraded_by_memory
    // 标记同步清理，避免重启后把用户选择误判为系统降级。
    void clearMqttMemoryOverrideState();
    bool isModbusStoppedForMemory() const { return _modbusStoppedForMemory; }
    bool isPeriphExecPausedForMemory() const { return _periphExecPausedForMemory; }
    // Worker 状态独立于 scheduler 暂停标志：worker 可能仍在退出，或在
    // 内存恢复时重新分配失败。低内存 compact 响应也要暴露这两个状态，
    // 避免页面误以为外设执行已经完全恢复。
    bool arePeriphExecWorkersSuspendedForMemory() const;
    bool isPeriphExecWorkerSuspensionPending() const;
    bool areOptionalPeripheralsSuspendedForMemory() const {
        return _optionalPeripheralsSuspendedForMemory;
    }
    uint16_t getMemorySuspendedPeripheralCount() const {
        return _memorySuspendedPeripheralCount;
    }
    unsigned long getCriticalDurationMs() const {
        return _criticalStartTime > 0 ? (millis() - _criticalStartTime) : 0;
    }

    // ── 外部模块上报指标 setter ──
    void setMqttQueueDepth(uint8_t depth);
    void setSseClientCount(uint8_t count);
    void setPollDurationMs(uint32_t ms);

    // 设置启动耗时（由 FastBeeFramework 在初始化完成后调用）
    void setBootTime(unsigned long bootMs);

    // 返回完整指标 JSON 字符串（兼容旧接口）
    String getMetricsJson();

    // 填充完整指标 JSON 到外部 String 缓冲（零额外堆分配，推荐路径）
    void fillMetricsJson(String& out);

    // ── 内存池统计注册（碎片预防监控）──
    struct PoolStatsEntry {
        const char* name;           // 池名称（如 "mqtt_publish"）
        uint16_t blockSize;         // 单块大小（字节）
        uint8_t  capacity;          // 总容量
        // 回调函数获取运行时指标
        uint8_t  (*getUsed)();      // 当前已用槽位
        uint32_t (*getExhaust)();   // 累计耗尽次数
    };
    static constexpr uint8_t MAX_TRACKED_POOLS = 4;
    void registerPool(const PoolStatsEntry& entry);
    void logPoolStats();  // 定期输出池使用率（在 logMetricsSummary 中调用）

private:
    void performHealthCheck();
    void initializeMemoryThresholds();
    void checkCriticalMemory();  // 低内存保护
    void logTaskStackWatermarks();  // 定期输出任务栈水位
    void logMetricsSummary();        // 定期串口输出关键指标

    SystemHealth  currentHealth;
    unsigned long lastCheckTime;
    unsigned long lastStackLogTime;   // 上次输出栈水位日志的时间
    unsigned long lastMetricsLogTime; // 上次输出指标摘要的时间
    uint32_t      heapWatermark;
    uint32_t      consecutiveLowMemCount;  // 连续低内存计数
    uint32_t      consecutiveFragmentationCriticalCount;  // sustained fragmentation counter
    bool          running;
    
    // 内存阈值（根据 DRAM 总量动态计算）
    uint32_t criticalThreshold;
    uint32_t warningThreshold;

    // MemGuard 运行期分级阈值。大多数目标使用 MemoryBudget 的通用值；
    // 经典 ESP32 无 PSRAM 的完整外设镜像使用较低的保护带，避免其
    // 已验证的 14~15KB 稳态被误判为 SEVERE 并暂停外设执行。
    uint32_t guardWarnDramThreshold = FastBee::MemoryBudget::GUARD_WARN_DRAM_FREE;
    uint32_t guardSevereDramThreshold = FastBee::MemoryBudget::GUARD_SEVERE_DRAM_FREE;
    uint32_t guardCriticalLargestThreshold = FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK;
    uint32_t guardSevereLargestThreshold = FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK;
    uint32_t guardWarnLargestThreshold = FastBee::MemoryBudget::GUARD_WARN_LARGEST_BLOCK;

    // 内存保护等级
    MemoryGuardLevel _currentLevel = MemoryGuardLevel::NORMAL;
    // 迟滞防振荡：S3 的非紧急 SEVERE 需连续 2 次采样，
    // CRITICAL 仍立即生效；恢复需连续 3 次达标。
    static constexpr uint8_t S3_SEVERE_ENTRY_CONFIRM_SAMPLES = 2;
    MemoryGuardLevel _lastComputedLevel = MemoryGuardLevel::NORMAL;
    uint8_t _consecutiveSeverePressureSamples = 0;
    uint8_t _consecutiveRecoverySamples = 0;
    bool _compactionTriggered = false;  // 碎片率紧凑化已触发标志
    void updateMemoryGuardLevel();  // 在 update() 中调用
    void applyDegradation(MemoryGuardLevel oldLevel, MemoryGuardLevel newLevel);  // 降级/恢复措施
    void compactMemory();           // 碎片率过高时触发紧凑化策略

    // G4: 主动碎片预检定时器（NORMAL/WARN 级别下定期轻量整理）
    uint32_t lastProactiveCompactTime = 0;
    static constexpr uint32_t PROACTIVE_COMPACT_INTERVAL = 1800000;  // 30 分钟
    static constexpr uint8_t  PROACTIVE_COMPACT_THRESHOLD = 50;       // 碎片率 50% 即触发轻量整理
    void proactiveCompactCheck();

    // 降级前保存的原始日志设置（用于恢复）
    LogLevel _savedLogLevel = LOG_INFO;
    bool     _savedFileLogging = true;
    bool     _degradationActive = false;  // 是否正处于降级状态

    // ── 内存恢复状态跟踪（防砖机制）──
    bool _mqttsMemoryDowngrade = false;      // Compatibility name: TLS paused, never plaintext downgrade.
    bool _mqttStoppedForMemory = false;      // MQTT 因内存不足被完全停止
    bool _mqttDisabledForMemory = false;     // Only a legacy persisted enabled=false marker; never newly written.
    bool _modbusStoppedForMemory = false;    // Modbus 因内存不足被停止
    bool _periphExecPausedForMemory = false; // PeriphExec 因内存不足被暂停
    bool _optionalPeripheralsSuspendedForMemory = false;
    uint16_t _memorySuspendedPeripheralCount = 0;
    bool _mqttRestorePendingForMemory = false; // MQTT 恢复待重试或尚未确认实例就绪
    bool _mqttRecoveryBlockedForStorage = false; // 准入存储不可用，不能用内存恢复重试替代重启
    unsigned long _mqttRestoreRetryAtMs = 0;   // MQTT 恢复失败后的退避时间
    unsigned long _criticalStartTime = 0;    // CRITICAL 级别开始时间（0=未进入）
    unsigned long _mqttsDowngradeTimeMs = 0;     // MQTTS 暂停时间戳（兼容名称）
    unsigned long _mqttsRestoreCooldownMs = 0;    // MQTTS 恢复冷却到期时间戳（24h 防风暴）
    unsigned long _mqttsDowngradeRetryAtMs = 0;   // TLS 暂停重试时间（兼容名称，不写配置）
    unsigned long _mqttsRestoreRetryAtMs = 0;     // TLS 读取配置/安全重建失败后的重试时间
    unsigned long _mqttDisableRetryAtMs = 0;      // MQTT 暂停重试时间（兼容名称，不写配置）
    bool _modbusRestorePendingForMemory = false;  // 已请求恢复，等待 loopTask 实际启动
    unsigned long _modbusRestoreRetryAtMs = 0;    // Modbus 恢复失败后的退避时间
    // 仅记录旧版降密元数据供诊断；未知端口为 0，不猜测、不自动回写磁盘。
    char _mqttsOriginalScheme[8] = "mqtts";
    uint16_t _mqttsOriginalPort = 0;
    static constexpr unsigned long CRITICAL_MQTT_STOP_DELAY_MS = 30000;   // 30s 后暂停 MQTT
    static constexpr unsigned long CRITICAL_REBOOT_DELAY_MS    = 90000;   // 90s 后重启
    unsigned long _guardCriticalSince = 0;   // MemGuard CRITICAL 开始时间（0=未进入）
    static constexpr unsigned long GUARD_CRITICAL_STUCK_MS = 600000;  // CRITICAL 降载持续 10 分钟无自愈则重启
    // 重启循环抑制窗口：若上次重启已是碎片压缩重启且开机未超此窗口又陷入
    // CRITICAL，说明碎片由外部流量维持、重启无法根治，不再重启避免死循环
    static constexpr unsigned long GUARD_REBOOT_LOOP_WINDOW_MS = 1800000;
    unsigned long _emergencySince = 0;       // DRAM<4KB EMERGENCY 开始时间（0=未进入）
    static constexpr unsigned long EMERGENCY_REBOOT_DELAY_MS = 5000;  // EMERGENCY 恢复无效持续 5s 则抢先优雅重启（防 OOM abort）
    static constexpr unsigned long MQTT_MEMORY_CONFIG_RETRY_MS = 30000;  // 配置读取或运行实例恢复失败后的退避
    static constexpr unsigned long MODBUS_MEMORY_RETRY_MS = 30000;       // 延迟重启失败后的退避

    void performMemoryRecovery(MemoryGuardLevel oldLevel, MemoryGuardLevel newLevel);
    void ensureTLSContiguousMemory();  // 确保 TLS 握手所需连续 DRAM（P0 优化）
    void downgradeMqttsToMqtt();      // Compatibility name: pause TLS without modifying user configuration.
    void restoreMqttsAfterMemory();  // Resume confirmed secure intent; no persistent scheme/port writes.
    void disableMqttForMemory();      // 本次启动暂停 MQTT，保留用户 enabled/传输配置
    void restoreMemoryRecovery();     // NORMAL 恢复时重启已停止的服务（MQTT 被禁用除外）
    void reconcilePendingMemoryRecovery(); // 确认延迟恢复成功后再清除状态
    bool refreshMqttRecoveryAdmission(); // 永久准入失败不触配置、不排队重建
    void loadPersistedMqttsDowngrade();  // 从 protocol.json 恢复跨重启的 MQTT 内存降级状态

    // 外部上报指标（预留接口）
    uint8_t  mqttQueueDepth;   // MQTT 队列深度
    uint8_t  sseClientCount;   // SSE 客户端数
    uint32_t pollDurationMs;   // 轮询耗时(ms)

    // ── 内存池注册表 ──
    PoolStatsEntry _trackedPools[MAX_TRACKED_POOLS];
    uint8_t _trackedPoolCount = 0;

    // ── DRAM 水位趋势追踪（泄漏检测）──
    static constexpr uint8_t WATERMARK_HISTORY_SIZE = 24;  // 保存 24 个小时采样点
    static constexpr unsigned long WATERMARK_SAMPLE_INTERVAL_MS = 3600000UL;  // 1 小时
    uint32_t _dramWatermarkHistory[WATERMARK_HISTORY_SIZE];  // 每小时 DRAM 最低水位
    uint8_t  _dramHistoryCount = 0;    // 已收集的采样数
    uint8_t  _dramHistoryIndex = 0;    // 环形缓冲区写入位置
    unsigned long _lastWatermarkSampleMs = 0;
    uint32_t _dramWatermarkSinceLastSample = UINT32_MAX;  // 当前采样周期内 DRAM 最低值
    bool detectMemoryLeak() const;  // 分析趋势判断是否有内存泄漏
};

#endif
