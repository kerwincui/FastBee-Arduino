#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "core/interfaces/ILoggerSystem.h"  // LogLevel 枚举

// 内存保护等级
enum class MemoryGuardLevel : uint8_t {
    NORMAL = 0,    // freeHeap >= 40KB → 所有功能正常
    WARN = 1,      // 40KB > freeHeap >= 25KB → 降低轮询频率、降低日志级别
    SEVERE = 2,    // 25KB > freeHeap >= 15KB → 暂停Modbus轮询、MQTT降采样、停止日志文件写
    CRITICAL = 3   // freeHeap < 15KB → 禁用文件日志、拒绝大响应、只保留关键页面
};

// 阈值常量
static constexpr uint32_t MEM_THRESHOLD_NORMAL  = 40960;  // 40KB
static constexpr uint32_t MEM_THRESHOLD_WARN    = 25600;  // 25KB
static constexpr uint32_t MEM_THRESHOLD_SEVERE  = 15360;  // 15KB
static constexpr uint8_t  FRAG_THRESHOLD_COMPACT = 75;    // 碎片率75%触发紧凑化

// 任务栈水位信息
struct TaskStackInfo {
    char name[16];
    uint32_t highWaterMark;  // 剩余栈空间（字节）
};

// 系统健康状态快照
struct SystemHealth {
    uint32_t     freeHeap;           // 当前空闲堆（字节）
    uint32_t     minFreeHeap;        // 历史最小空闲堆（字节）
    uint8_t      heapFragmentation;  // 堆碎片率（0-100%）
    bool         fileSystemOK;       // 文件系统是否正常
    bool         wifiConnected;      // WiFi 是否已连接
    int8_t       wifiStrength;       // WiFi RSSI（dBm）
    unsigned long uptime;            // 系统运行时间（毫秒）
    uint8_t      cpuUsage;           // CPU 估算占用（0-100%）
    uint32_t     largestFreeBlock;   // 最大连续可用块（字节）
    unsigned long bootTimeMs;        // 启动耗时（毫秒）
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

    // ── 外部模块上报指标 setter ──
    void setMqttQueueDepth(uint8_t depth);
    void setSseClientCount(uint8_t count);
    void setPollDurationMs(uint32_t ms);

    // 设置启动耗时（由 FastBeeFramework 在初始化完成后调用）
    void setBootTime(unsigned long bootMs);

    // 返回完整指标 JSON 字符串
    String getMetricsJson();

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

    // 外部上报指标（预留接口）
    uint8_t  mqttQueueDepth;   // MQTT 队列深度
    uint8_t  sseClientCount;   // SSE 客户端数
    uint32_t pollDurationMs;   // 轮询耗时(ms)
};

#endif
