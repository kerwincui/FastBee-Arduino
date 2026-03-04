// main.cpp
#include "core/FastBeeFramework.h"
#include <esp_heap_caps.h>

FastBeeFramework* framework;

void setup() {
    Serial.begin(115200);
    delay(500);

    // 启动诊断：输出可用内存，快速判断 PSRAM / 堆是否正常
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