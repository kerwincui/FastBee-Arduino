#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "utils/HeapFragmentation.h"
#include "core/MemoryBudget.h"
#include "core/interfaces/ILoggerSystem.h"  // LogLevel 枚举

// 内存保护等级（ESP32 D0WD-V3 no-PSRAM 实测：WiFi+WebServer+外设常态吃掉 ~210KB，
// 剩余 25~30KB 是稳定工作区间，不应被判为警报）
enum class MemoryGuardLevel : uint8_t {
    NORMAL = 0,    // freeHeap >= 20KB 或 largestBlock >= 12KB → 所有功能正常
    WARN = 1,      // 20KB > freeHeap >= 10KB → 降低轮询频率、降低日志级别
    SEVERE = 2,    // 10KB > freeHeap >= 6KB → 暂停Modbus轮询、MQTT降采样、停止日志文件写
    CRITICAL = 3   // freeHeap < 6KB → 禁用文件日志、拒绝大响应、只保留关键页面
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
    uint32_t highWaterMark;  // 剩余栈空间（字节）
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

    // 获取关键任务栈水位信息
    size_t getTaskStackInfo(TaskStackInfo* outInfo, size_t maxTasks) const;

    // ── 内存保护等级 ──
    MemoryGuardLevel getMemoryGuardLevel() const;
    bool isMemoryNormal() const;
    bool isMemoryWarning() const;
    bool isMemorySevere() const;
    bool isMemoryCritical() const;
    bool isFragmentationHigh() const;  // 碎片率>75%

    // ── 内存恢复状态查询（供 API 和外部模块使用）──
    bool isMqttsDowngraded() const { return _mqttsMemoryDowngrade; }
    bool isMqttStoppedForMemory() const { return _mqttStoppedForMemory; }
    bool isMqttDisabledForMemory() const { return _mqttDisabledForMemory; }
    bool isModbusStoppedForMemory() const { return _modbusStoppedForMemory; }
    bool isPeriphExecPausedForMemory() const { return _periphExecPausedForMemory; }
    unsigned long getCriticalDurationMs() const {
        return _criticalStartTime > 0 ? (millis() - _criticalStartTime) : 0;
    }

    // ── 外部模块上报指标 setter ──
    void setMqttQueueDepth(uint8_t depth);
    void setSseClientCount(uint8_t count);
    void setPollDurationMs(uint32_t ms);

    // 设置启动耗时（由 FastBeeFramework 在初始化完成后调用）
    void setBootTime(unsigned long bootMs);

    // 返回完整指标 JSON 字符串
    String getMetricsJson();

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

    // 内存保护等级
    MemoryGuardLevel _currentLevel = MemoryGuardLevel::NORMAL;
    bool _compactionTriggered = false;  // 碎片率紧凑化已触发标志
    void updateMemoryGuardLevel();  // 在 update() 中调用
    void applyDegradation(MemoryGuardLevel oldLevel, MemoryGuardLevel newLevel);  // 降级/恢复措施
    void compactMemory();           // 碎片率过高时触发紧凑化策略

    // 降级前保存的原始日志设置（用于恢复）
    LogLevel _savedLogLevel = LOG_INFO;
    bool     _savedFileLogging = true;
    bool     _degradationActive = false;  // 是否正处于降级状态

    // ── 内存恢复状态跟踪（防砖机制）──
    bool _mqttsMemoryDowngrade = false;      // MQTTS 已降级为 MQTT（scheme 已写入配置文件）
    bool _mqttStoppedForMemory = false;      // MQTT 因内存不足被完全停止
    bool _mqttDisabledForMemory = false;     // MQTT 因内存不足被永久禁用（写入配置 enabled=false）
    bool _modbusStoppedForMemory = false;    // Modbus 因内存不足被停止
    bool _periphExecPausedForMemory = false; // PeriphExec 因内存不足被暂停
    unsigned long _criticalStartTime = 0;    // CRITICAL 级别开始时间（0=未进入）
    static constexpr unsigned long CRITICAL_MQTT_STOP_DELAY_MS = 30000;   // 30s 后禁用 MQTT
    static constexpr unsigned long CRITICAL_REBOOT_DELAY_MS    = 90000;   // 90s 后重启

    void performMemoryRecovery(MemoryGuardLevel oldLevel, MemoryGuardLevel newLevel);
    void downgradeMqttsToMqtt();      // 将 scheme 从 mqtts 切换为 mqtt 并重启 MQTT
    void disableMqttForMemory();      // 写入 mqtt.enabled=false 到 protocol.json + stopMQTT
    void restoreMemoryRecovery();     // NORMAL 恢复时重启已停止的服务（MQTT 被禁用除外）

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
