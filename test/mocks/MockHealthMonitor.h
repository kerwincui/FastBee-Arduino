/**
 * @file MockHealthMonitor.h
 * @brief 健康监控模拟对象
 * 
 * 提供系统健康监控功能的模拟实现
 */

#ifndef MOCK_HEALTH_MONITOR_H
#define MOCK_HEALTH_MONITOR_H

#include <Arduino.h>
#include <vector>
#include <map>

// 健康状态结构
struct SystemHealth {
    // 内存信息
    uint32_t freeHeap;
    uint32_t totalHeap;
    uint32_t minFreeHeap;
    uint32_t maxAllocHeap;
    
    // 文件系统信息
    size_t fsTotalBytes;
    size_t fsUsedBytes;
    size_t fsFreeBytes;
    
    // WiFi信息
    bool wifiConnected;
    int8_t wifiStrength;  // RSSI
    String wifiSSID;
    String localIP;
    
    // 系统信息
    uint32_t uptime;      // 运行时间（秒）
    float cpuUsage;       // CPU使用率（百分比）
    float temperature;    // 芯片温度
    
    // 任务信息
    int taskCount;
    int runningTasks;
    
    // 健康状态
    bool isHealthy;
    std::vector<String> warnings;
    std::vector<String> errors;
    
    SystemHealth() : freeHeap(0), totalHeap(0), minFreeHeap(0), maxAllocHeap(0),
                     fsTotalBytes(0), fsUsedBytes(0), fsFreeBytes(0),
                     wifiConnected(false), wifiStrength(0),
                     uptime(0), cpuUsage(0.0f), temperature(0.0f),
                     taskCount(0), runningTasks(0), isHealthy(true) {}
};

// 健康检查项
struct HealthCheckItem {
    String name;
    String description;
    bool passed;
    String message;
    float threshold;
    float currentValue;
    
    HealthCheckItem() : passed(true), threshold(0.0f), currentValue(0.0f) {}
};

// 模拟健康监控器
class MockHealthMonitor {
public:
    static MockHealthMonitor& getInstance() {
        static MockHealthMonitor instance;
        return instance;
    }

    bool initialize() {
        _initialized = true;
        _checkInterval = 30000;  // 默认30秒检查一次
        _lastCheck = 0;
        _health = SystemHealth();
        _wifiOverride = false;
        _heapOverride = false;
        _fsOverride = false;
        _checks.clear();
        return true;
    }

    // 更新健康状态
    void update() {
        unsigned long now = millis();
        
        // 模拟内存信息（尊重显式设置的堆值）
        _health.totalHeap = 327680;  // 320KB
        if (!_heapOverride) {
            _health.freeHeap = random(50000, 150000);
        }
        _health.minFreeHeap = random(30000, 50000);
        _health.maxAllocHeap = random(40000, 80000);
        
        // 模拟文件系统信息
        if (!_fsOverride) {
            _health.fsTotalBytes = 1024 * 1024;  // 1MB
            _health.fsUsedBytes = random(100000, 500000);
            _health.fsFreeBytes = _health.fsTotalBytes - _health.fsUsedBytes;
        }
        
        // WiFi状态（允许测试显式覆盖）
        if (!_wifiOverride) {
            _health.wifiConnected = random(0, 2) == 1;
            _health.wifiStrength = _health.wifiConnected ? random(-80, -30) : 0;
            _health.wifiSSID = _health.wifiConnected ? "TestWiFi" : "";
            _health.localIP = _health.wifiConnected ? "192.168.1.100" : "0.0.0.0";
        }
        
        // 系统信息
        _health.uptime = now / 1000;
        _health.cpuUsage = random(0, 100) / 10.0f;
        _health.temperature = random(200, 600) / 10.0f;  // 20-60度
        
        // 任务信息
        _health.taskCount = random(5, 20);
        _health.runningTasks = random(1, 5);
        
        // 执行健康检查
        performHealthChecks();
        
        _lastCheck = now;
    }

    // 获取健康状态
    SystemHealth getHealthStatus() {
        return _health;
    }

    // 生成健康报告
    size_t getHealthReport(char* buffer, size_t bufferSize) {
        String report = "System Health Report:\n";
        report += "==================\n";
        report += "heap=" + String(_health.freeHeap) + "\n";
        report += "Heap: " + String(_health.freeHeap) + "/" + 
                  String(_health.totalHeap) + " bytes free\n";
        report += "FS: " + String(_health.fsFreeBytes) + "/" + 
                  String(_health.fsTotalBytes) + " bytes free\n";
        report += "WiFi: " + String(_health.wifiConnected ? "Connected" : "Disconnected") + "\n";
        report += "Uptime: " + String(_health.uptime) + " seconds\n";
        report += "CPU: " + String(_health.cpuUsage, 1) + "%\n";
        report += "Temp: " + String(_health.temperature, 1) + "C\n";
        report += "Tasks: " + String(_health.runningTasks) + "/" + 
                  String(_health.taskCount) + "\n";
        report += "Status: " + String(_health.isHealthy ? "HEALTHY" : "UNHEALTHY") + "\n";
        
        if (!_health.warnings.empty()) {
            report += "\nWarnings:\n";
            for (auto& warning : _health.warnings) {
                report += "  - " + warning + "\n";
            }
        }
        
        if (!_health.errors.empty()) {
            report += "\nErrors:\n";
            for (auto& error : _health.errors) {
                report += "  - " + error + "\n";
            }
        }
        
        size_t len = report.length();
        if (len >= bufferSize) len = bufferSize - 1;
        
        strncpy(buffer, report.c_str(), len);
        buffer[len] = '\0';
        
        return len;
    }

    // 检查特定指标
    bool checkHeapMemory(uint32_t minFreeBytes = 10000) {
        return _health.freeHeap >= minFreeBytes;
    }

    bool checkFSSpace(size_t minFreeBytes = 50000) {
        return _health.fsFreeBytes >= minFreeBytes;
    }

    bool checkWiFiConnection() {
        return _health.wifiConnected;
    }

    bool checkTemperature(float maxTemp = 80.0f) {
        return _health.temperature < maxTemp;
    }

    // 添加自定义检查
    void addHealthCheck(const HealthCheckItem& check) {
        _checks.push_back(check);
    }

    std::vector<HealthCheckItem> getHealthChecks() {
        return _checks;
    }

    // 获取警告和错误
    std::vector<String> getWarnings() {
        return _health.warnings;
    }

    std::vector<String> getErrors() {
        return _health.errors;
    }

    // 设置检查间隔
    void setCheckInterval(unsigned long intervalMs) {
        _checkInterval = intervalMs;
    }

    unsigned long getCheckInterval() {
        return _checkInterval;
    }

    // 测试辅助方法
    void setWiFiConnected(bool connected) {
        _wifiOverride = true;
        _health.wifiConnected = connected;
        if (!connected) {
            _health.wifiStrength = 0;
            _health.wifiSSID = "";
            _health.localIP = "0.0.0.0";
        } else {
            _health.wifiStrength = -45;
            _health.wifiSSID = "TestWiFi";
            _health.localIP = "192.168.1.100";
        }
    }

    void setFreeHeap(uint32_t heap) {
        _health.freeHeap = heap;
        _heapOverride = true;
    }

    void setFSSpace(size_t used, size_t total) {
        _health.fsUsedBytes = used;
        _health.fsTotalBytes = total;
        _health.fsFreeBytes = total - used;
        _fsOverride = true;
    }

    void addWarning(const String& warning) {
        _health.warnings.push_back(warning);
        _health.isHealthy = false;
    }

    void addError(const String& error) {
        _health.errors.push_back(error);
        _health.isHealthy = false;
    }

    void clearWarningsAndErrors() {
        _health.warnings.clear();
        _health.errors.clear();
        _health.isHealthy = true;
    }

private:
    MockHealthMonitor() : _initialized(false), _checkInterval(30000), 
                          _lastCheck(0), _wifiOverride(false), _heapOverride(false),
                          _fsOverride(false) {}

    void performHealthChecks() {
        _health.warnings.clear();
        _health.errors.clear();
        _health.isHealthy = true;
        
        // 内存检查
        if (_health.freeHeap < 20000) {
            _health.warnings.push_back("Low heap memory: " + 
                                       String(_health.freeHeap) + " bytes");
        }
        if (_health.freeHeap < 10000) {
            _health.errors.push_back("Critical heap memory: " + 
                                     String(_health.freeHeap) + " bytes");
            _health.isHealthy = false;
        }
        
        // 文件系统检查
        float fsUsage = (float)_health.fsUsedBytes / _health.fsTotalBytes * 100;
        if (fsUsage > 80) {
            _health.warnings.push_back("File system usage high: " + 
                                       String(fsUsage, 1) + "%");
        }
        if (fsUsage > 95) {
            _health.errors.push_back("File system nearly full: " + 
                                     String(fsUsage, 1) + "%");
            _health.isHealthy = false;
        }
        
        // 温度检查
        if (_health.temperature > 70) {
            _health.warnings.push_back("High temperature: " + 
                                       String(_health.temperature, 1) + "C");
        }
        if (_health.temperature > 85) {
            _health.errors.push_back("Critical temperature: " + 
                                     String(_health.temperature, 1) + "C");
            _health.isHealthy = false;
        }
        
        // 自定义检查
        for (auto& check : _checks) {
            if (!check.passed) {
                _health.warnings.push_back(check.name + ": " + check.message);
            }
        }
    }

    bool _initialized;
    unsigned long _checkInterval;
    unsigned long _lastCheck;
    bool _wifiOverride;
    bool _heapOverride;
    bool _fsOverride;
    SystemHealth _health;
    std::vector<HealthCheckItem> _checks;
};

// 全局实例引用
#define MockHealthMon MockHealthMonitor::getInstance()

#endif // MOCK_HEALTH_MONITOR_H
