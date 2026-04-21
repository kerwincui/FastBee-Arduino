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

HealthMonitor::HealthMonitor()
    : lastCheckTime(0),
      lastStackLogTime(0),
      heapWatermark(UINT32_MAX),
      consecutiveLowMemCount(0),
      running(false) {
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
        checkCriticalMemory();
        lastCheckTime = currentTime;
    }
    // 任务栈水位日志：每 60 秒输出一次
    if (currentTime - lastStackLogTime >= 60000UL) {
        logTaskStackWatermarks();
        lastStackLogTime = currentTime;
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
