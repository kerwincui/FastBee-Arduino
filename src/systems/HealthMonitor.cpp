/**
 *@description: 
 *@author: kerwincui
 *@copyright:FastBee All rights reserved.
 *@date: 2025-12-02 17:32:41
 */

#include "systems/HealthMonitor.h"
#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <core/SystemConstants.h>


HealthMonitor::HealthMonitor() {
    lastCheckTime = 0;
    heapWatermark = UINT32_MAX;
}

bool HealthMonitor::initialize() {
    performHealthCheck();
    return true;
}

void HealthMonitor::update() {
    unsigned long currentTime = millis();
    if (currentTime - lastCheckTime >= 5000) { // 每5秒检查一次
        performHealthCheck();
        lastCheckTime = currentTime;
    }
}

void HealthMonitor::performHealthCheck() {
    // 内存状态
    currentHealth.freeHeap = esp_get_free_heap_size();
    currentHealth.minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    
    // 更新堆水位标记
    if (currentHealth.freeHeap < heapWatermark) {
        heapWatermark = currentHealth.freeHeap;
    }
    
    // 估算堆碎片（简化计算）
    size_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (currentHealth.freeHeap > 0) {
        currentHealth.heapFragmentation = 100 - (largestFreeBlock * 100 / currentHealth.freeHeap);
    } else {
        currentHealth.heapFragmentation = 0;
    }
    
    // 文件系统状态
    currentHealth.fileSystemOK = LittleFS.exists("/config/system.json");
    
    // 网络状态
    currentHealth.wifiConnected = WiFi.status() == WL_CONNECTED;
    currentHealth.wifiStrength = WiFi.RSSI();
    
    // 系统运行时间
    currentHealth.uptime = millis();
    
    // CPU使用率估算（基于空闲任务）
    // 注意：这是一个简化估算，实际需要更复杂的计算
    currentHealth.cpuUsage = 0; // 简化处理
}

String HealthMonitor::getHealthReport() {
    String report = "System Health Report:\n";
    report += "  Free Heap: " + String(currentHealth.freeHeap) + " bytes\n";
    report += "  Min Free Heap: " + String(currentHealth.minFreeHeap) + " bytes\n";
    report += "  Heap Fragmentation: " + String(currentHealth.heapFragmentation) + "%\n";
    report += "  File System: " + String(currentHealth.fileSystemOK ? "OK" : "ERROR") + "\n";
    report += "  WiFi: " + String(currentHealth.wifiConnected ? "Connected" : "Disconnected");
    if (currentHealth.wifiConnected) {
        report += " (RSSI: " + String(currentHealth.wifiStrength) + " dBm)";
    }
    report += "\n";
    report += "  Uptime: " + String(currentHealth.uptime / 1000) + " seconds\n";
    
    return report;
}

bool HealthMonitor::isSystemHealthy() {
    return currentHealth.freeHeap >= HealthCheck::MIN_FREE_HEAP &&
           currentHealth.heapFragmentation <= HealthCheck::MAX_HEAP_FRAGMENTATION &&
           currentHealth.fileSystemOK;
}