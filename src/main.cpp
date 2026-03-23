// main.cpp
#include "core/FastBeeFramework.h"
#include <esp_heap_caps.h>
#include <esp_bt.h>

FastBeeFramework* framework;

void setup() {
    // 关键输出引脚早期初始化：在任何外设初始化之前将隔离型IO拉低，
    // 防止启动阶段因引脚浮空导致误触发。
    // 先设置输出寄存器再切换模式，避免 pinMode(OUTPUT) 瞬间的电平毛刺。
    digitalWrite(21, LOW);   // IO/L 隔离型数字输出低端
    digitalWrite(22, LOW);   // IO/H 隔离型数字输出高端
    pinMode(21, OUTPUT);
    pinMode(22, OUTPUT);

    Serial.begin(115200);
    delay(500);

    // 释放蓝牙控制器内存（~30KB），BLE配网需要时重启后启用
    // 通过 device.json 的 "bleReserve":true 可保留BT内存
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);

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