#ifndef HEALTH_MONITOR_H
#define HEALTH_MONITOR_H

#include <Arduino.h>

struct SystemHealth {
    uint32_t freeHeap;
    uint32_t minFreeHeap;
    uint8_t heapFragmentation;
    bool fileSystemOK;
    bool wifiConnected;
    int wifiStrength;
    unsigned long uptime;
    uint8_t cpuUsage; // 估算值
};

class HealthMonitor {
private:
    SystemHealth currentHealth;
    unsigned long lastCheckTime;
    uint32_t heapWatermark;
    
    void performHealthCheck();
    
public:
    HealthMonitor();
    bool initialize();
    void update();
    
    SystemHealth getHealthStatus() { return currentHealth; }
    String getHealthReport();
    bool isSystemHealthy();
};

#endif