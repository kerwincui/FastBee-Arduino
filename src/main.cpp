// main.cpp
#include "core/FastBeeFramework.h"
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include <esp_chip_info.h>
#include <esp_task_wdt.h>
#include <exception>
#include <cstdlib>

FastBeeFramework* framework;

// 全局异常终止处理器：作为最后防线，防止 uncaught exception → abort()
// 场景：库代码（FS/NVS/ArduinoJson）抛出异常但未被捕获时触发
static void fastbee_terminate_handler() {
    Serial.println();
    Serial.println("===== FATAL: Uncaught Exception =====");
    Serial.printf("Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    
    // 尝试获取当前异常信息（C++ 标准不支持，但 ESP32 GCC 可能输出有用信息）
    // 无法安全获取异常类型，只记录堆栈和内存
    
    Serial.println("System will reboot in 3 seconds...");
    Serial.println("=====================================");
    Serial.flush();
    delay(3000);
    ESP.restart();
}

void setup() {
    Serial.begin(115200);
    delay(500);

    // 安装全局异常终止处理器（必须在任何可能抛异常的代码之前）
    std::set_terminate(fastbee_terminate_handler);

    // 释放蓝牙控制器内存（~30KB），BLE配网需要时重启后启用
    // 通过 device.json 的 "bleReserve":true 可保留BT内存
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

    // 启动诊断：输出芯片信息和可用内存，快速判断 PSRAM / 堆是否正常
    Serial.printf("[BOOT] Chip: %s Rev%d\n", ESP.getChipModel(), ESP.getChipRevision());
    Serial.printf("[BOOT] Flash: %luKB, PSRAM: %luKB\n",
                  (unsigned long)(ESP.getFlashChipSize() / 1024),
                  (unsigned long)(ESP.getPsramSize() / 1024));
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    Serial.printf("[BOOT] Cores: %d, Features: WiFi%s%s\n",
                  chip_info.cores,
                  (chip_info.features & CHIP_FEATURE_BT) ? "/BT" : "",
                  (chip_info.features & CHIP_FEATURE_BLE) ? "/BLE" : "");
    Serial.printf("[BOOT] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("[BOOT] Max alloc: %lu bytes\n", (unsigned long)ESP.getMaxAllocHeap());
#ifdef BOARD_HAS_PSRAM
    Serial.printf("[BOOT] PSRAM size: %lu bytes\n", (unsigned long)ESP.getPsramSize());
    Serial.printf("[BOOT] Free PSRAM: %lu bytes\n", (unsigned long)ESP.getFreePsram());
#else
    Serial.println("[BOOT] PSRAM: disabled (no-PSRAM build)");
#endif

    // 增大任务看门狗超时为 10 秒，防止 async_tcp/loopTask 在执行文件 I/O 或大型 JSON 序列化时触发 WDT
    esp_task_wdt_init(10, true);

    framework = FastBeeFramework::getInstance();

    // 使用 try/catch 包裹初始化，防止库代码异常导致 abort()
    try {
        if (!framework->initialize()) {
            Serial.println("[FATAL] Failed to initialize FastBee framework!");
            Serial.printf("[FATAL] Remaining heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
            while (1) { delay(1000); }
        }
    } catch (const std::bad_alloc& e) {
        Serial.println("[FATAL] Out of memory during initialization!");
        Serial.printf("[FATAL] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
        while (1) { delay(1000); }
    } catch (const std::exception& e) {
        Serial.printf("[FATAL] Exception during init: %s\n", e.what());
        Serial.printf("[FATAL] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
        while (1) { delay(1000); }
    } catch (...) {
        Serial.println("[FATAL] Unknown exception during initialization!");
        Serial.printf("[FATAL] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
        while (1) { delay(1000); }
    }
}

void loop() {
    // 运行框架主循环
    framework->run();
    
    // 喝狗：确保 loopTask 不触发 WDT
    esp_task_wdt_reset();
    
    // 让出 CPU 时间，平衡响应和功耗，提升稳定性
    delay(10); 
}