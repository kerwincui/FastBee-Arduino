// main.cpp
#include "core/FastBeeFramework.h"

FastBeeFramework* framework;

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // 获取框架实例并初始化
    framework = FastBeeFramework::getInstance();
    
    if (!framework->initialize()) {
        Serial.println("Failed to initialize FastBee framework!");
        while (1) {
            delay(1000);
        }
    }
}

void loop() {
    // 运行框架主循环
    framework->run();
    
    // 让出CPU时间，平衡响应和功耗，提升稳定性
    delay(10); 
}