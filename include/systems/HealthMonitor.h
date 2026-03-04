#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <Arduino.h>

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

private:
    void performHealthCheck();

    SystemHealth  currentHealth;
    unsigned long lastCheckTime;
    uint32_t      heapWatermark;
    bool          running;
};

#endif
