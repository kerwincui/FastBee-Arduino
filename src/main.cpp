// main.cpp
#include "core/FastBeeFramework.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include <esp_heap_caps.h>
#include <esp_bt.h>
#include <esp_chip_info.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <exception>
#include <cstdlib>

FastBeeFramework* framework;

// ----------------------------------------------------------------------------
// 覆盖 Arduino-ESP32 框架的 weak 符号：提升 loopTask 栈到 24KB
// 原因：Arduino-ESP32 框架的 main.cpp 被 PlatformIO 预编译为 libFrameworkArduino.a，
//       platformio.ini 里的 -DARDUINO_LOOP_STACK_SIZE 对已编译的 main.o 无效。
//       框架在 cores/esp32/main.cpp 把 getArduinoLoopTaskStackSize() 标记为 __attribute__((weak))，
//       在用户代码里重新实现即可覆盖，真正放大 loopTask 栈。
// 场景：Modbus JSON 模式 registerModbusSubDevices 循环注册多个子设备，
//       每次 addPeripheral -> initHardware -> setupHardware 调用链深，
//       默认 8KB 栈不够 → Double Exception (EXCCAUSE=2, EXCVADDR=0x00fffffc)。
// 24KB 预留足够裕度，以 ~16KB DRAM 换栈溢出防护，值得。
// ----------------------------------------------------------------------------
extern "C++" size_t getArduinoLoopTaskStackSize(void);
size_t getArduinoLoopTaskStackSize(void) {
    return 24 * 1024;  // 24KB (默认 8KB)
}

// 全局异常终止处理器：作为最后防线，防止 uncaught exception → abort()
// 场景：库代码（FS/NVS/ArduinoJson）抛出异常但未被捕获时触发
static void fastbee_terminate_handler() {
    Serial.println();
    Serial.println("===== FATAL: Uncaught Exception =====");
    Serial.printf("Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    
    // 保存重启前状态快照到 RTC 内存
    RestartDiagnostics::savePreRestartState(
        RestartReason::UNCAUGHT_EXCEPTION,
        "std::terminate() — uncaught exception");
    
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
    // ESP32-C6 只有 BLE 无 Classic
#if defined(CONFIG_IDF_TARGET_ESP32)
    esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
    esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
#endif

    // ======== 启动诊断：检测重启原因 ========
    // 必须在 Serial 初始化后尽早调用，输出上次重启的完整诊断信息
    RestartDiagnostics::logBootDiagnostics();

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
    // 启用 PSRAM 分配策略：≥ 256字节的分配优先使用 PSRAM
    // 为什么 256 而不是 512？
    // - ESP32-S3 内部 DRAM 仅 ~320KB，WiFi+MQTT+WebServer 后只剩 ~18KB
    // - AsyncWebServer 每个 HTTP 请求缓冲区 ~1-2KB，4096 阈值太高无法卸载到 PSRAM
    // - lwIP TCP PCB (~200B) 和 FreeRTOS 栈必须在内部 DRAM，其它大部分分配可用 PSRAM
    // - PSRAM 延迟比 DRAM 高 ~3x，但对 HTTP/JSON 响应构建无感知影响
    // - 256 比 512 更积极地将中等分配卸载到 PSRAM，为 MQTT/TLS 释放更多 DRAM
    heap_caps_malloc_extmem_enable(256);  // ≥ 256B 的分配请求优先用 PSRAM
    Serial.println("[BOOT] PSRAM malloc enabled (threshold=256)");
#else
    Serial.println("[BOOT] PSRAM: disabled (no-PSRAM build)");
#endif

    // 重新配置任务看门狗（Task Watchdog Timer）
    // ESP-IDF v5 框架默认启用 TWDT（5s超时），AsyncTCP 库内部会调用 esp_task_wdt_reset()
    // 不能 deinit（否则库调用 reset 会报错），只需加大超时防止长操作触发复位
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdt_cfg = {
        .timeout_ms = 60000,       // 60秒超时（默认5秒太短）
        .idle_core_mask = 0,       // 不监控空闲任务
        .trigger_panic = false     // 超时不触发panic，只打印警告
    };
    esp_err_t err = esp_task_wdt_reconfigure(&wdt_cfg);
    if (err == ESP_OK) {
        Serial.println("[WDT] Task watchdog reconfigured: 60s timeout");
    } else {
        Serial.printf("[WDT] Reconfigure failed (0x%x), deinit fallback\n", err);
        esp_task_wdt_deinit();
    }
#else
    esp_task_wdt_init(60, false);  // false = 不触发 panic
    Serial.println("[WDT] Task watchdog timeout set to 60s");
#endif

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
    // 运行框架主循环（异常保护：捕获非致命异常避免整个设备重启）
    try {
        framework->run();
    } catch (const std::bad_alloc& e) {
        // 内存分配失败：跳过本周期，让 MemGuard 处理内存恢复
        static unsigned long _badAllocWarn = 0;
        if (millis() - _badAllocWarn > 30000) {
            _badAllocWarn = millis();
            Serial.printf("[LOOP] bad_alloc caught, heap=%lu — skipping cycle\n",
                          (unsigned long)ESP.getFreeHeap());
        }
    } catch (const std::exception& e) {
        // 标准异常：记录日志并计数，连续异常触发安全重启
        static uint32_t _loopExceptionCount = 0;
        _loopExceptionCount++;
        static unsigned long _excWarn = 0;
        if (millis() - _excWarn > 10000) {
            _excWarn = millis();
            Serial.printf("[LOOP] Exception in run() (#%lu): %s\n",
                          (unsigned long)_loopExceptionCount, e.what());
        }
        // 连续 10 次异常说明系统性故障，调度安全重启
        if (_loopExceptionCount >= 10 && !SystemRebooter::isScheduled()) {
            Serial.println("[LOOP] 10 consecutive exceptions, scheduling safe reboot");
            SystemRebooter::scheduleReboot("loop_exception_loop", 3000,
                                            RestartReason::UNCAUGHT_EXCEPTION);
        }
    } catch (...) {
        // 未知异常：最危险的情况，立即调度重启
        static uint32_t _unknownExcCount = 0;
        _unknownExcCount++;
        Serial.printf("[LOOP] Unknown exception in run() (#%lu)\n",
                      (unsigned long)_unknownExcCount);
        if (_unknownExcCount >= 3 && !SystemRebooter::isScheduled()) {
            Serial.println("[LOOP] 3 consecutive unknown exceptions, scheduling reboot");
            SystemRebooter::scheduleReboot("loop_unknown_exception", 3000,
                                            RestartReason::UNCAUGHT_EXCEPTION);
        }
    }
    
    // 启动稳定后（120s），将 TWDT 升级为 trigger_panic=true
    // 启动阶段允许 WDT 超时仅告警（库初始化可能耗时），稳定后死锁必须触发重启
    static bool _wdtPanicUpgraded = false;
    if (!_wdtPanicUpgraded && millis() > 120000) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        esp_task_wdt_config_t wdt_cfg_panic = {
            .timeout_ms = 60000,
            .idle_core_mask = 0,
            .trigger_panic = true
        };
        if (esp_task_wdt_reconfigure(&wdt_cfg_panic) == ESP_OK) {
            Serial.println("[WDT] Upgraded to trigger_panic=true (stable boot confirmed)");
            _wdtPanicUpgraded = true;
        }
#else
        esp_task_wdt_init(60, true);
        Serial.println("[WDT] Upgraded to trigger_panic=true (stable boot confirmed)");
        _wdtPanicUpgraded = true;
#endif
    }

    // 周期性确认 loop 运行
    static unsigned long _loopDbg = 0;
    if (millis() - _loopDbg > 15000) {
        _loopDbg = millis();
        ets_printf("[LOOP-ALIVE] heap=%lu\n", (unsigned long)ESP.getFreeHeap());
    }
    
    // 让出 CPU 时间，平衡响应和功耗，提升稳定性
    delay(10); 
}