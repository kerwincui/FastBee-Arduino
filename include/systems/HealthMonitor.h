#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

private:
    void performHealthCheck();
    void checkCriticalMemory();  // 低内存保护
    void logTaskStackWatermarks();  // 定期输出任务栈水位

    SystemHealth  currentHealth;
    unsigned long lastCheckTime;
    unsigned long lastStackLogTime;  // 上次输出栈水位日志的时间
    uint32_t      heapWatermark;
    uint32_t      consecutiveLowMemCount;  // 连续低内存计数
    bool          running;
};

#endif
