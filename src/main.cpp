// main.cpp
#include "core/FastBeeFramework.h"
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include <esp_chip_info.h>

FastBeeFramework* framework;

void setup() {
    Serial.begin(115200);
    delay(500);

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

    framework = FastBeeFramework::getInstance();

    if (!framework->initialize()) {
        Serial.println("[FATAL] Failed to initialize FastBee framework!");
        Serial.printf("[FATAL] Remaining heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
        while (1) { delay(1000); }
    }
}

void loop() {
    // 运行框架主循环
    framework->run();
    
    // 让出CPU时间，平衡响应和功耗，提升稳定性
    delay(10); 
}