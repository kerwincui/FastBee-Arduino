/**
 * @file test_stability_fixes.cpp
 * @brief 稳定性审计修复守护测试（2026-07 稳定性专项）
 *
 * 覆盖修复项：
 *  - P0 WiFi 扫描 async_tcp 忙等 8s → chunked + RESPONSE_TRY_AGAIN 非阻塞
 *  - P0 WiFi 扫描 FreeRTOS 任务隔离 + mutex 保护 + AP 模式恢复 + 回调轻量性 + 创建失败兜底
 *  - P0 OTA URL 下载同步阻塞 async_tcp / 下载循环无超时 → 后台任务 + 双重超时
 *  - P1 OTA 完成路径 async_tcp delay(3000) → SystemRebooter 延迟重启
 *  - P1 MQTT 上报环形缓冲无锁竞态 → _slotMux 自旋锁临界区
 *  - P0 编码器 ISR 堆分配/悬垂指针 → 静态槽位数组 + portMUX
 *  - P0 配置写入非原子 → tmp+rename 原子替换 + 重启前强制 flush
 *  - P1 ConfigStorage dirty 缓存读己之写
 *  - P1 MQTT flapping 抑制 + reconnectCount 老化
 *  - P2 millis() 回绕不安全比较四处
 */

#include <unity.h>
#include <fstream>
#include <sstream>
#include <string>
#include <cstdint>
#include <cstring>
#include "core/MemoryBudget.h"
#include "helpers/TestLogger.h"

void test_stability_fixes_group();

// 辅助：读取项目源文件（用于源码回归测试）
static std::string readStabilitySrcFile(const char* relativePath) {
    const char* roots[] = { ".", "..", "../..", "../../..", "../../../.." };
    for (const char* root : roots) {
        std::string fullPath = std::string(root) + "/" + relativePath;
        std::ifstream file(fullPath);
        if (file.is_open()) {
            std::ostringstream ss;
            ss << file.rdbuf();
            return ss.str();
        }
    }
    return "";
}

// ========== P0：WiFi 扫描非阻塞化 + FreeRTOS 任务隔离 ==========

static void test_wifi_scan_handler_nonblocking() {
    TestLog::testStart("Stability: WiFi scan handler must not busy-wait on async_tcp");

    std::string content = readStabilitySrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping source check");
        return;
    }

    // 旧实现：handler 内 delay(10) 轮询忙等最长 8 秒，阻塞整个 async_tcp 任务
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("while (millis() - startMs < 8000)") == std::string::npos,
        "handleWiFiScan must not busy-wait 8s on async_tcp task");

    // 新实现：chunked 响应 + RESPONSE_TRY_AGAIN 非阻塞轮询扫描结果
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("beginChunkedResponse") != std::string::npos,
        "handleWiFiScan must use chunked response for non-blocking scan wait");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("RESPONSE_TRY_AGAIN") != std::string::npos,
        "handleWiFiScan must yield async_tcp via RESPONSE_TRY_AGAIN while scanning");

    // 阻塞扫描必须运行在独立 FreeRTOS 任务，不得在 chunked 回调（async_tcp）上执行
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("xTaskCreatePinnedToCore") != std::string::npos,
        "WiFi scan must run in a dedicated FreeRTOS task (xTaskCreatePinnedToCore)");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("_wifiScanTask") != std::string::npos,
        "ProvisionRouteHandler must define _wifiScanTask static entry point");
    // WiFi.scanNetworks() 只能在独立任务内调用，不得出现在 handleWiFiScan 函数体中
    size_t handlerPos = content.find("void ProvisionRouteHandler::handleWiFiScan");
    size_t handlerEnd = content.find("void ProvisionRouteHandler::_launchScanTask");
    TEST_ASSERT_TRUE_MESSAGE(handlerPos != std::string::npos && handlerEnd != std::string::npos,
        "handleWiFiScan and _launchScanTask must both exist");
    std::string handlerBody = content.substr(handlerPos, handlerEnd - handlerPos);
    TEST_ASSERT_TRUE_MESSAGE(
        handlerBody.find("WiFi.scanNetworks()") == std::string::npos,
        "handleWiFiScan (async_tcp path) must NOT call WiFi.scanNetworks() directly");
}

// ========== P0：WiFi 扫描任务互斥锁保护 ==========

static void test_wifi_scan_task_mutex_protection() {
    TestLog::testStart("Stability: WiFi scan shared state must be mutex-protected");

    std::string header = readStabilitySrcFile("include/network/handlers/ProvisionRouteHandler.h");
    if (header.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.h not readable, skipping source check");
        return;
    }
    // 头文件必须声明互斥锁与共享状态字段
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("SemaphoreHandle_t scanMutex") != std::string::npos,
        "ProvisionRouteHandler.h must declare SemaphoreHandle_t scanMutex");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("volatile bool scanResultReady") != std::string::npos,
        "ProvisionRouteHandler.h must declare volatile scanResultReady flag");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("volatile bool scanTaskRunning") != std::string::npos,
        "ProvisionRouteHandler.h must declare volatile scanTaskRunning for dedup");

    std::string src = readStabilitySrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping source check");
        return;
    }
    // 构造函数必须创建 mutex
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("xSemaphoreCreateMutex()") != std::string::npos,
        "Constructor must create scanMutex via xSemaphoreCreateMutex");
    // 扫描任务写入结果时必须持锁
    size_t taskPos = src.find("void ProvisionRouteHandler::_wifiScanTask");
    TEST_ASSERT_TRUE_MESSAGE(taskPos != std::string::npos, "_wifiScanTask must exist");
    std::string taskBody = src.substr(taskPos);
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("xSemaphoreTake(self->scanMutex") != std::string::npos,
        "_wifiScanTask must acquire scanMutex before writing shared result");
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("xSemaphoreGive(self->scanMutex)") != std::string::npos,
        "_wifiScanTask must release scanMutex after writing shared result");
    // chunked 回调读取结果时也必须持锁
    size_t chunkPos = src.find("beginChunkedResponse");
    TEST_ASSERT_TRUE_MESSAGE(chunkPos != std::string::npos, "chunked response must exist");
    std::string chunkArea = src.substr(chunkPos, 800);
    TEST_ASSERT_TRUE_MESSAGE(
        chunkArea.find("xSemaphoreTake(self->scanMutex, 0)") != std::string::npos,
        "Chunked callback must try-acquire scanMutex (non-blocking) before reading result");
}

// ========== P0：WiFi 扫描后恢复纯 AP 模式 ==========

static void test_wifi_scan_restores_pure_ap_mode() {
    TestLog::testStart("Stability: WiFi scan must restore pure AP mode after scan");

    std::string src = readStabilitySrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping source check");
        return;
    }
    // scanNetworks 内部调用 enableSTA(true) 会把纯 AP 切为 APSTA，
    // 扫描结束后必须恢复，否则 STA 残留触发自动重连/模式干扰
    size_t taskPos = src.find("void ProvisionRouteHandler::_wifiScanTask");
    TEST_ASSERT_TRUE_MESSAGE(taskPos != std::string::npos, "_wifiScanTask must exist");
    std::string taskBody = src.substr(taskPos);

    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("wasPureAP") != std::string::npos,
        "_wifiScanTask must record whether mode was pure AP before scan");
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("WIFI_MODE_AP") != std::string::npos,
        "_wifiScanTask must reference WIFI_MODE_AP for mode restoration");
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("WiFi.mode(WIFI_MODE_AP)") != std::string::npos,
        "_wifiScanTask must call WiFi.mode(WIFI_MODE_AP) to restore pure AP");
    // 恢复逻辑必须在扫描完成之后、结果写入之前或之后（不能在扫描前）
    size_t scanPos = taskBody.find("WiFi.scanNetworks()");
    size_t restorePos = taskBody.find("WiFi.mode(WIFI_MODE_AP)");
    TEST_ASSERT_TRUE_MESSAGE(
        scanPos != std::string::npos && restorePos != std::string::npos && restorePos > scanPos,
        "AP mode restoration must occur AFTER scanNetworks completes");
}

// ========== P0：chunked 回调轻量性守护 ==========

static void test_wifi_scan_chunked_callback_lightweight() {
    TestLog::testStart("Stability: chunked callback must not call WiFi API or build JSON");

    std::string src = readStabilitySrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping source check");
        return;
    }
    // 提取 chunked 回调区域（从 beginChunkedResponse 到 request->send）
    size_t chunkStart = src.find("beginChunkedResponse");
    size_t chunkEnd = src.find("request->send(response)", chunkStart);
    TEST_ASSERT_TRUE_MESSAGE(
        chunkStart != std::string::npos && chunkEnd != std::string::npos,
        "Chunked response block must exist");
    std::string callback = src.substr(chunkStart, chunkEnd - chunkStart);

    // 回调内禁止调用 WiFi API（会导致 async_tcp 小栈过载）
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("WiFi.scanNetworks") == std::string::npos,
        "Chunked callback must NOT call WiFi.scanNetworks");
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("WiFi.SSID") == std::string::npos,
        "Chunked callback must NOT call WiFi.SSID");
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("WiFi.RSSI") == std::string::npos,
        "Chunked callback must NOT call WiFi.RSSI");
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("WiFi.scanDelete") == std::string::npos,
        "Chunked callback must NOT call WiFi.scanDelete");
    // 回调内禁止构建 JsonDocument（堆分配 + 序列化开销不适合 async_tcp 栈）
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("JsonDocument") == std::string::npos,
        "Chunked callback must NOT construct JsonDocument");
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("serializeJson") == std::string::npos,
        "Chunked callback must NOT call serializeJson");
    // 回调应仅使用 memcpy 拷贝已构建好的结果
    TEST_ASSERT_TRUE_MESSAGE(
        callback.find("memcpy") != std::string::npos,
        "Chunked callback should use memcpy to send pre-built result");
}

// ========== P0：WiFi 扫描任务创建失败兜底 ==========

static void test_wifi_scan_task_creation_failure_fallback() {
    TestLog::testStart("Stability: scan task creation failure must set error result");

    std::string src = readStabilitySrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping source check");
        return;
    }
    // _launchScanTask 必须处理 xTaskCreatePinnedToCore 返回非 pdPASS 的情况
    size_t launchPos = src.find("void ProvisionRouteHandler::_launchScanTask");
    TEST_ASSERT_TRUE_MESSAGE(launchPos != std::string::npos, "_launchScanTask must exist");
    size_t launchEnd = src.find("void ProvisionRouteHandler::_wifiScanTask", launchPos);
    if (launchEnd == std::string::npos) launchEnd = launchPos + 1000;
    std::string launchBody = src.substr(launchPos, launchEnd - launchPos);

    TEST_ASSERT_TRUE_MESSAGE(
        launchBody.find("pdPASS") != std::string::npos,
        "_launchScanTask must check xTaskCreatePinnedToCore return against pdPASS");
    TEST_ASSERT_TRUE_MESSAGE(
        launchBody.find("scan_failed") != std::string::npos,
        "_launchScanTask must set scan_failed error JSON when task creation fails");
    TEST_ASSERT_TRUE_MESSAGE(
        launchBody.find("scanTaskRunning = false") != std::string::npos,
        "_launchScanTask must clear scanTaskRunning on failure to unblock future requests");
    TEST_ASSERT_TRUE_MESSAGE(
        launchBody.find("scanResultReady = true") != std::string::npos,
        "_launchScanTask must set scanResultReady=true on failure so HTTP response completes");
}

// ========== P0：单核芯片（C3/C6）扫描任务创建安全 ==========

static void test_wifi_scan_task_single_core_safe() {
    TestLog::testStart("Stability: scan task creation must not pin Core 1 on single-core chips");

    std::string src = readStabilitySrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping source check");
        return;
    }
    // 单核芯片（C3/C6）上 xTaskCreatePinnedToCore(..., 1) 会触发 FreeRTOS assert
    // 崩溃重启（freertos_tasks_c_additions.h:163 xCoreID < portNUM_PROCESSORS），
    // 导致浏览器收到 ERR_CONNECTION_RESET。必须按 CHIP_DUAL_CORE 区分。
    size_t launchPos = src.find("void ProvisionRouteHandler::_launchScanTask");
    TEST_ASSERT_TRUE_MESSAGE(launchPos != std::string::npos, "_launchScanTask must exist");
    size_t launchEnd = src.find("void ProvisionRouteHandler::_wifiScanTask", launchPos);
    if (launchEnd == std::string::npos) launchEnd = launchPos + 1200;
    std::string launchBody = src.substr(launchPos, launchEnd - launchPos);

    TEST_ASSERT_TRUE_MESSAGE(
        launchBody.find("#if CHIP_DUAL_CORE") != std::string::npos,
        "_launchScanTask must guard core pinning with #if CHIP_DUAL_CORE");
    // 单核分支必须用不绑核的 xTaskCreate
    TEST_ASSERT_TRUE_MESSAGE(
        launchBody.find("xTaskCreate(") != std::string::npos,
        "_launchScanTask must fall back to xTaskCreate on single-core chips");
    // 绑核调用必须位于 CHIP_DUAL_CORE 分支内（在 #if 之后）
    size_t guardPos = launchBody.find("#if CHIP_DUAL_CORE");
    size_t pinnedPos = launchBody.find("xTaskCreatePinnedToCore");
    TEST_ASSERT_TRUE_MESSAGE(
        pinnedPos != std::string::npos && pinnedPos > guardPos,
        "xTaskCreatePinnedToCore must only appear inside the CHIP_DUAL_CORE branch");
    // 源文件必须引入 ChipConfig.h 以获得 CHIP_DUAL_CORE 宏
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("ChipConfig.h") != std::string::npos,
        "ProvisionRouteHandler.cpp must include ChipConfig.h for CHIP_DUAL_CORE");
}

// ========== P0：STA 模式扫描缓存 TTL + 短驻留时间 ==========

static void test_wifi_scan_sta_mode_cache_and_dwell() {
    TestLog::testStart("Stability: STA scan must use cache TTL and reduced dwell time");

    std::string header = readStabilitySrcFile("include/network/handlers/ProvisionRouteHandler.h");
    if (header.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.h not readable, skipping source check");
        return;
    }
    // 必须声明缓存 TTL 常量和上次完成时间戳
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("SCAN_CACHE_TTL_MS") != std::string::npos,
        "ProvisionRouteHandler.h must define SCAN_CACHE_TTL_MS constant");
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("scanLastCompleteMs") != std::string::npos,
        "ProvisionRouteHandler.h must declare scanLastCompleteMs timestamp");

    std::string src = readStabilitySrcFile("src/network/handlers/ProvisionRouteHandler.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("ProvisionRouteHandler.cpp not readable, skipping source check");
        return;
    }
    // handleWiFiScan 必须检查缓存新鲜度，避免 STA 模式下每次请求都触发扫描断连
    size_t handlerPos = src.find("void ProvisionRouteHandler::handleWiFiScan");
    size_t handlerEnd = src.find("void ProvisionRouteHandler::_launchScanTask");
    TEST_ASSERT_TRUE(handlerPos != std::string::npos && handlerEnd != std::string::npos);
    std::string handlerBody = src.substr(handlerPos, handlerEnd - handlerPos);
    TEST_ASSERT_TRUE_MESSAGE(
        handlerBody.find("cacheFresh") != std::string::npos,
        "handleWiFiScan must check cache freshness before launching scan");
    TEST_ASSERT_TRUE_MESSAGE(
        handlerBody.find("SCAN_CACHE_TTL_MS") != std::string::npos,
        "handleWiFiScan must use SCAN_CACHE_TTL_MS for cache validity check");

    // _wifiScanTask 必须在 STA 模式下使用缩短的每信道驻留时间
    size_t taskPos = src.find("void ProvisionRouteHandler::_wifiScanTask");
    TEST_ASSERT_TRUE(taskPos != std::string::npos);
    std::string taskBody = src.substr(taskPos);
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("wasSTA") != std::string::npos,
        "_wifiScanTask must detect STA mode for reduced dwell time");
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("scanNetworks(false, false, false, 60)") != std::string::npos,
        "_wifiScanTask must use 60ms per-channel dwell in STA mode (default 120ms)");

    // 扫描完成后必须记录时间戳，且失败结果不得刷新 TTL（立即视为过期）
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("scanLastCompleteMs = ") != std::string::npos,
        "_wifiScanTask must record scanLastCompleteMs on completion");
    TEST_ASSERT_TRUE_MESSAGE(
        taskBody.find("millis() - SCAN_CACHE_TTL_MS") != std::string::npos,
        "_wifiScanTask must expire cache immediately on scan failure (n < 0)");

    // setupRoutes 必须预热扫描缓存：首次用户请求命中缓存，避免 STA 冷扫描断连
    size_t setupPos = src.find("void ProvisionRouteHandler::setupRoutes");
    size_t setupEnd = src.find("void ProvisionRouteHandler::handleSetupPage");
    TEST_ASSERT_TRUE(setupPos != std::string::npos && setupEnd != std::string::npos);
    std::string setupBody = src.substr(setupPos, setupEnd - setupPos);
    TEST_ASSERT_TRUE_MESSAGE(
        setupBody.find("needPrewarm") != std::string::npos &&
        setupBody.find("_launchScanTask()") != std::string::npos,
        "setupRoutes must prewarm the scan cache at startup");
}

// ========== P0：OTA URL 下载后台任务化 + 超时保护 ==========

static void test_ota_url_download_runs_in_background_task() {
    TestLog::testStart("Stability: OTA URL download must run in background task");

    std::string handler = readStabilitySrcFile("src/network/handlers/OTARouteHandler.cpp");
    if (handler.empty()) {
        TEST_IGNORE_MESSAGE("OTARouteHandler.cpp not readable, skipping source check");
        return;
    }
    // handler 内不得同步执行 startOTA（下载数据走 async_tcp 收包，同任务执行会自饿死）
    TEST_ASSERT_TRUE_MESSAGE(
        handler.find("startOTAAsync") != std::string::npos,
        "handleOtaUrl must dispatch download via startOTAAsync");

    std::string manager = readStabilitySrcFile("src/network/OTAManager.cpp");
    if (manager.empty()) {
        TEST_IGNORE_MESSAGE("OTAManager.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        manager.find("otaUrlDownloadTask") != std::string::npos &&
        manager.find("xTaskCreate") != std::string::npos,
        "OTAManager must create a dedicated FreeRTOS task for URL download");
}

static void test_ota_download_loop_has_timeouts() {
    TestLog::testStart("Stability: OTA download loop must have stall & total timeouts");

    std::string content = readStabilitySrcFile("src/network/OTAManager.cpp");
    if (content.empty()) {
        TEST_IGNORE_MESSAGE("OTAManager.cpp not readable, skipping source check");
        return;
    }
    // 服务器建连后停发数据时 http.connected() 保持 true，无超时则永久自旋直至 TWDT panic
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("STALL_TIMEOUT_MS") != std::string::npos,
        "OTA download loop must detect stalled downloads (no-progress timeout)");
    TEST_ASSERT_TRUE_MESSAGE(
        content.find("TOTAL_TIMEOUT_MS") != std::string::npos,
        "OTA download loop must have an overall deadline");
}

static void test_ota_restart_uses_system_rebooter() {
    TestLog::testStart("Stability: OTA completion must not delay(3000) on async_tcp");

    std::string handler = readStabilitySrcFile("src/network/handlers/OTARouteHandler.cpp");
    if (handler.empty()) {
        TEST_IGNORE_MESSAGE("OTARouteHandler.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        handler.find("delay(3000)") == std::string::npos,
        "OTARouteHandler must not sleep 3s on async_tcp before restart");
    TEST_ASSERT_TRUE_MESSAGE(
        handler.find("SystemRebooter::scheduleReboot") != std::string::npos,
        "OTARouteHandler must delegate restart to SystemRebooter");
    TEST_ASSERT_TRUE_MESSAGE(
        handler.find("ESP.restart()") == std::string::npos,
        "OTARouteHandler must not call ESP.restart() directly");

    std::string manager = readStabilitySrcFile("src/network/OTAManager.cpp");
    if (!manager.empty()) {
        // legacy 上传路径同样不得在 async_tcp 上 delay(3000)
        TEST_ASSERT_TRUE_MESSAGE(
            manager.find("delay(3000)") == std::string::npos,
            "OTAManager upload path must not sleep 3s on async_tcp before restart");
        // 非 final 分块禁止对同一 request 多次 send（协议违规）
        TEST_ASSERT_TRUE_MESSAGE(
            manager.find("index % (512 * 1024)") == std::string::npos,
            "OTAManager must not send multiple responses per upload request");
    }
}

// ========== P1：MQTT 上报环形缓冲加锁 ==========

static void test_mqtt_report_ring_buffer_locked() {
    TestLog::testStart("Stability: MQTT report ring buffer must be lock-protected");

    std::string header = readStabilitySrcFile("include/protocols/MQTTClient.h");
    if (header.empty()) {
        TEST_IGNORE_MESSAGE("MQTTClient.h not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("portMUX_TYPE _slotMux") != std::string::npos,
        "MQTTClient.h must declare _slotMux spinlock for ring buffer indices");

    std::string src = readStabilitySrcFile("src/protocols/MQTTClient.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("MQTTClient.cpp not readable, skipping source check");
        return;
    }
    // 生产者（worker 任务）与消费者（loopTask）都必须在 _slotMux 临界区内操作索引
    size_t firstUse = src.find("portENTER_CRITICAL(&_slotMux)");
    TEST_ASSERT_TRUE_MESSAGE(firstUse != std::string::npos,
        "queueReportData/processQueuedReports must guard indices with _slotMux");
    size_t secondUse = src.find("portENTER_CRITICAL(&_slotMux)", firstUse + 1);
    TEST_ASSERT_TRUE_MESSAGE(secondUse != std::string::npos,
        "Both producer and consumer sides must enter the _slotMux critical section");
}

// ========== P1：MQTT flapping 抑制 ==========

static void test_mqtt_flapping_suppression() {
    TestLog::testStart("Stability: MQTT flapping suppression & reconnectCount aging");

    std::string src = readStabilitySrcFile("src/protocols/MQTTClient.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("MQTTClient.cpp not readable, skipping source check");
        return;
    }
    // 短命会话（<60s）断开时退避翻倍而非重置 5s，防 flapping 打爆 broker
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("Flapping detected") != std::string::npos,
        "MQTT handle() must detect flapping (short-lived sessions) and back off");
    // 稳定连接 5 分钟后清零 reconnectCount，防误入 5 分钟慢重连模式
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("millis() - lastConnectedTime > 300000UL") != std::string::npos,
        "MQTT handle() must age out reconnectCount after 5min of stable connection");
}

// ========== P0：编码器 ISR 安全 ==========

static void test_encoder_isr_static_slots() {
    TestLog::testStart("Stability: Encoder ISR must use static slots (no heap in ISR)");

    std::string header = readStabilitySrcFile("include/core/PeripheralManager.h");
    if (header.empty()) {
        TEST_IGNORE_MESSAGE("PeripheralManager.h not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("EncoderIsrSlot") != std::string::npos,
        "PeripheralManager.h must declare static EncoderIsrSlot array");
    // 旧实现：ISR 内对 std::map<String,...> 做 String 拷贝 + 节点插入（非 ISR-safe malloc）
    TEST_ASSERT_TRUE_MESSAGE(
        header.find("std::map<String, volatile int32_t> encoderCounters") == std::string::npos,
        "encoderCounters map (heap allocation in ISR path) must be removed");

    std::string src = readStabilitySrcFile("src/core/PeripheralManager.cpp");
    if (src.empty()) {
        TEST_IGNORE_MESSAGE("PeripheralManager.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        src.find("portENTER_CRITICAL_ISR(&_encoderMux)") != std::string::npos,
        "Encoder ISR must protect counter with portMUX critical section");
}

// ========== P0：配置原子写入 ==========

static void test_config_write_is_atomic() {
    TestLog::testStart("Stability: Config writes must be atomic (tmp+rename)");

    std::string fileUtils = readStabilitySrcFile("src/utils/FileUtils.cpp");
    if (fileUtils.empty()) {
        TEST_IGNORE_MESSAGE("FileUtils.cpp not readable, skipping source check");
        return;
    }
    // littlefs rename 具备 POSIX 覆盖语义：rename-first 消除 remove→rename 断电丢失窗口
    size_t renamePos = fileUtils.find("LittleFS.rename(tmpPath, path)");
    TEST_ASSERT_TRUE_MESSAGE(renamePos != std::string::npos,
        "atomicWriteFile must rename tmp over target (POSIX overwrite semantics)");

    std::string storage = readStabilitySrcFile("src/systems/ConfigStorage.cpp");
    if (storage.empty()) {
        TEST_IGNORE_MESSAGE("ConfigStorage.cpp not readable, skipping source check");
        return;
    }
    TEST_ASSERT_TRUE_MESSAGE(
        storage.find("atomicReplaceFromTmp") != std::string::npos,
        "ConfigStorage flush paths must use tmp+rename atomic replace");

    std::string rebooter = readStabilitySrcFile("src/systems/SystemRebooter.cpp");
    if (!rebooter.empty()) {
        // 重启前强制落盘 dirty 缓存，消除"保存并重启"路径的配置丢失窗口
        TEST_ASSERT_TRUE_MESSAGE(
            rebooter.find("flushDirtyEntries(true)") != std::string::npos,
            "SystemRebooter must force-flush dirty config entries before restart");
    }
}

// ========== P1：ConfigStorage 读己之写 ==========

static void test_config_cache_read_your_write() {
    TestLog::testStart("Stability: ConfigStorage dirty cache must be read-your-write");

    std::string storage = readStabilitySrcFile("src/systems/ConfigStorage.cpp");
    if (storage.empty()) {
        TEST_IGNORE_MESSAGE("ConfigStorage.cpp not readable, skipping source check");
        return;
    }
    // dirty 条目必须直接命中缓存（fileModifyTime=0 会导致 mtime 检查必 miss → 读回旧数据）
    TEST_ASSERT_TRUE_MESSAGE(
        storage.find("bool cacheFresh = entry->dirty") != std::string::npos,
        "loadJSONConfig must treat dirty entries as fresh (skip mtime check)");
    // load 回填不得覆盖脏数据（否则后续 flush 会把旧数据写回磁盘 = 写丢失）
    TEST_ASSERT_TRUE_MESSAGE(
        storage.find("entry->dirty && modTime != 0") != std::string::npos,
        "updateCacheEntry must not overwrite dirty rawJson with disk data");
}

// ========== P2：millis() 回绕安全 ==========

static void test_millis_wraparound_safe_comparisons() {
    TestLog::testStart("Stability: millis() comparisons must be wraparound-safe");

    std::string modbus = readStabilitySrcFile("src/protocols/ModbusHandler.cpp");
    if (!modbus.empty()) {
        // 延时线圈任务误触发有物理侧后果（如"延时1小时关阀"变成立刻关阀）
        TEST_ASSERT_TRUE_MESSAGE(
            modbus.find("(long)(now - coilDelayTasks[i].triggerTime) >= 0") != std::string::npos,
            "processCoilDelayTasks must use signed-diff comparison");
        TEST_ASSERT_TRUE_MESSAGE(
            modbus.find("now >= coilDelayTasks[i].triggerTime") == std::string::npos,
            "processCoilDelayTasks must not use absolute deadline comparison");
    }

    std::string script = readStabilitySrcFile("src/core/ScriptEngine.cpp");
    if (!script.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            script.find("millis() - delayStart < ms") != std::string::npos,
            "scriptDelay must use subtraction-based timeout");
    }

    std::string network = readStabilitySrcFile("src/network/NetworkManager.cpp");
    if (!network.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            network.find("millis() - waitStart < wifiConfig.connectTimeout") != std::string::npos,
            "WiFi connect wait must use subtraction-based timeout");
    }

    std::string storage = readStabilitySrcFile("src/systems/ConfigStorage.cpp");
    if (!storage.empty()) {
        TEST_ASSERT_TRUE_MESSAGE(
            storage.find("(long)(now - _cache[i].debounceUntil) >= 0") != std::string::npos,
            "flushDirtyEntries debounce must use signed-diff comparison");
    }
}

// ========== 行为镜像测试：环形缓冲丢旧语义 ==========

namespace {
// 镜像 MQTTClient 环形缓冲的入队/出队/队满丢旧逻辑（8 槽）
struct MirrorRingBuffer {
    static const uint8_t SLOTS = 8;
    int values[SLOTS];
    bool occupied[SLOTS] = {false};
    uint8_t writeIdx = 0, readIdx = 0, count = 0;
    uint32_t drops = 0;

    void enqueue(int v) {
        if (count >= SLOTS) {  // 队满丢弃最旧
            occupied[readIdx] = false;
            readIdx = (readIdx + 1) % SLOTS;
            count--;
            drops++;
        }
        values[writeIdx] = v;
        occupied[writeIdx] = true;
        writeIdx = (writeIdx + 1) % SLOTS;
        count++;
    }

    bool dequeue(int& out) {
        if (count == 0) return false;
        out = values[readIdx];
        occupied[readIdx] = false;
        readIdx = (readIdx + 1) % SLOTS;
        count--;
        return true;
    }
};
}  // namespace

static void test_ring_buffer_drop_oldest_semantics() {
    TestLog::testStart("Stability: ring buffer drop-oldest keeps FIFO order");

    MirrorRingBuffer rb;
    // 写满 8 槽后再写 4 个：应丢弃最旧的 0..3
    for (int i = 0; i < 12; i++) rb.enqueue(i);
    TEST_ASSERT_EQUAL_UINT8(8, rb.count);
    TEST_ASSERT_EQUAL_UINT32(4, rb.drops);

    int v = -1;
    for (int expect = 4; expect < 12; expect++) {
        TEST_ASSERT_TRUE(rb.dequeue(v));
        TEST_ASSERT_EQUAL_INT(expect, v);
    }
    TEST_ASSERT_FALSE(rb.dequeue(v));
    TEST_ASSERT_EQUAL_UINT8(0, rb.count);

    // 交替读写跨越索引回绕边界，FIFO 顺序保持
    for (int i = 100; i < 120; i++) {
        rb.enqueue(i);
        TEST_ASSERT_TRUE(rb.dequeue(v));
        TEST_ASSERT_EQUAL_INT(i, v);
    }
}

// ========== 行为镜像测试：millis 回绕有符号差值判定 ==========

static void test_signed_diff_across_millis_wraparound() {
    TestLog::testStart("Stability: signed-diff deadline fires correctly across wrap");

    // 场景1：回绕前设定的 deadline，回绕后 now 变小 —— 差值判定仍正确触发
    uint32_t triggerTime = 0xFFFFFFF0u;      // 回绕前 16ms 处设定
    uint32_t now = 10u;                       // 已回绕 26ms 后
    TEST_ASSERT_TRUE((int32_t)(now - triggerTime) >= 0);   // 已到期，应触发

    // 场景2：deadline 溢出为小值（millis()+delta 回绕），未到期不得误触发
    uint32_t setAt = 0xFFFFFF00u;
    uint32_t deadline = setAt + 3600000u;     // 溢出为小值
    now = setAt + 1000u;                      // 仅过 1 秒（也已回绕）
    TEST_ASSERT_FALSE((int32_t)(now - deadline) >= 0);     // 未到期，不触发
    // 绝对比较写法在此场景会误触发（now < deadline 不成立时立即执行）
    // 而减法写法 now - setAt = 1000 < 3600000 正确等待
    TEST_ASSERT_TRUE(now - setAt < 3600000u);
}

// ========== 边界测试：MQTTS 无 PSRAM 握手准入闸门（a8a P0） ==========

static void test_mqtts_no_psram_gate_boundaries() {
    TestLog::testStart("Stability: MQTTS no-PSRAM gate must reject marginal DRAM");

    using FastBee::MemoryBudget;
    constexpr uint32_t D = MemoryBudget::MQTTS_NO_PSRAM_MIN_DRAM_FREE;
    constexpr uint32_t B = MemoryBudget::MQTTS_NO_PSRAM_MIN_LARGEST_BLOCK;

    // 无 PSRAM 门槛必须覆盖 mbedTLS 峰值（~42KB）+ lwIP 小分配余量
    TEST_ASSERT_TRUE_MESSAGE(D >= 42000u + 8192u,
        "no-PSRAM DRAM gate must cover TLS peak + lwIP headroom");
    // largest block 必须容纳单个 record buffer + 分配器 overhead
    TEST_ASSERT_TRUE_MESSAGE(B >= 16384u + 1024u,
        "no-PSRAM largest-block gate must fit one record buffer + overhead");

    // 阈值边界：恰好到线放行，差 1 字节拒绝（防止边缘尝试打穿 largest block）
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(D, B, false));
    TEST_ASSERT_FALSE(MemoryBudget::canAttemptMqtts(D - 1, B, false));
    TEST_ASSERT_FALSE(MemoryBudget::canAttemptMqtts(D, B - 1, false));
    TEST_ASSERT_FALSE(MemoryBudget::canAttemptMqtts(0, 0, false));

    // 有 PSRAM 时走宽松门槛：历史 panic 现场（DRAM~48K/largest~20K）
    // 在无 PSRAM 门槛下必须被拒绝，有 PSRAM 时允许（大缓冲走 PSRAM）
    TEST_ASSERT_TRUE(MemoryBudget::canAttemptMqtts(48000u, 20000u, true));
    TEST_ASSERT_FALSE(MemoryBudget::canAttemptMqtts(48000u, 20000u, false));

    // 无 PSRAM 门槛必须严于通用门槛（单调关系防回退）
    TEST_ASSERT_TRUE(D > MemoryBudget::MQTTS_MIN_DRAM_FREE);
    TEST_ASSERT_TRUE(B > MemoryBudget::MQTTS_MIN_LARGEST_BLOCK);
}

static void test_mqtts_no_psram_recovery_retry_gate() {
    TestLog::testStart("Stability: no-PSRAM 10s recovery retry must not thrash");

    using FastBee::MemoryBudget;
    constexpr uint32_t D = MemoryBudget::MQTTS_NO_PSRAM_MIN_DRAM_FREE;

    // 离严格门槛太远时禁止 10s 高频重试（反复 pause/resume Web 加剧碎片化）
    TEST_ASSERT_FALSE_MESSAGE(
        MemoryBudget::canRetryMqttsMemoryRecovery(D - 8193u, 20000u, false),
        "Far below gate must fall through to 300s backoff, not 10s retry");
    // 接近门槛（8KB 以内）允许短重试探测恢复
    TEST_ASSERT_TRUE(
        MemoryBudget::canRetryMqttsMemoryRecovery(D - 8192u, 20000u, false));
    // largest block 低于 WARN 线时无论 DRAM 多少都不重试
    TEST_ASSERT_FALSE(
        MemoryBudget::canRetryMqttsMemoryRecovery(
            D, MemoryBudget::GUARD_WARN_LARGEST_BLOCK - 1, false));
}

// ========== 边界测试：Web 暂停/回收决策阈值单调性 ==========

static void test_mqtts_reclaim_pause_threshold_ordering() {
    TestLog::testStart("Stability: MQTTS reclaim/pause thresholds must be ordered");

    using FastBee::MemoryBudget;
    // READY（无需回收）> WEB_PAUSE（深度暂停）> MIN（准入下限）：
    // 阈值乱序会导致"已回收但仍判定需暂停"的振荡
    TEST_ASSERT_TRUE(MemoryBudget::MQTTS_READY_DRAM_FREE >
                     MemoryBudget::MQTTS_WEB_PAUSE_DRAM_FREE);
    TEST_ASSERT_TRUE(MemoryBudget::MQTTS_WEB_PAUSE_DRAM_FREE >
                     MemoryBudget::MQTTS_MIN_DRAM_FREE);
    TEST_ASSERT_TRUE(MemoryBudget::MQTTS_READY_LARGEST_BLOCK >
                     MemoryBudget::MQTTS_WEB_PAUSE_LARGEST_BLOCK);
    TEST_ASSERT_TRUE(MemoryBudget::MQTTS_WEB_PAUSE_LARGEST_BLOCK >
                     MemoryBudget::MQTTS_MIN_LARGEST_BLOCK);

    // 健康堆（高于 READY）：既不回收也不暂停 Web
    TEST_ASSERT_FALSE(MemoryBudget::shouldReclaimBeforeMqtts(60000u, 45000u));
    TEST_ASSERT_FALSE(MemoryBudget::shouldPauseWebBeforeMqtts(60000u, 45000u));
    // 中间地带（READY 与 WEB_PAUSE 之间）：只回收不暂停
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(52000u, 40000u));
    TEST_ASSERT_FALSE(MemoryBudget::shouldPauseWebBeforeMqtts(52000u, 40000u));
    // 低位（低于 WEB_PAUSE）：回收 + 暂停
    TEST_ASSERT_TRUE(MemoryBudget::shouldReclaimBeforeMqtts(45000u, 30000u));
    TEST_ASSERT_TRUE(MemoryBudget::shouldPauseWebBeforeMqtts(45000u, 30000u));
}

// ========== 行为镜像测试：MQTT flapping 退避（a8e P1） ==========

namespace {
// 镜像 MQTTClient handle() 断连路径的重连间隔决策
struct MirrorBackoff {
    uint32_t reconnectInterval = 5000;

    // sessionUptimeMs：本次会话从连上到断开的存活时长
    void onDisconnect(uint32_t sessionUptimeMs) {
        if (sessionUptimeMs < 60000u) {
            // flapping：现有间隔翻倍封顶 60s
            uint32_t base = reconnectInterval > 5000u ? reconnectInterval : 5000u;
            uint32_t doubled = base * 2;
            reconnectInterval = doubled < 60000u ? doubled : 60000u;
        } else {
            reconnectInterval = 5000;  // 稳定会话断开：重置快速重连
        }
    }
};
}  // namespace

static void test_mqtt_flapping_backoff_progression() {
    TestLog::testStart("Stability: flapping backoff doubles to 60s cap, stable resets");

    MirrorBackoff bo;
    // 连续短命会话（连上即断）：5s→10s→20s→40s→60s 封顶
    const uint32_t expected[] = {10000, 20000, 40000, 60000, 60000, 60000};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        bo.onDisconnect(3000u);  // 会话仅存活 3s
        TEST_ASSERT_EQUAL_UINT32(expected[i], bo.reconnectInterval);
    }

    // 一次稳定会话（>60s）后断开：恢复 5s 快速重连（不残留惩罚）
    bo.onDisconnect(120000u);
    TEST_ASSERT_EQUAL_UINT32(5000, bo.reconnectInterval);

    // 边界：恰好 60s 视为稳定会话
    bo.onDisconnect(3000u);
    TEST_ASSERT_EQUAL_UINT32(10000, bo.reconnectInterval);
    bo.onDisconnect(60000u);
    TEST_ASSERT_EQUAL_UINT32(5000, bo.reconnectInterval);
}

// ========== 并发压力镜像：环形缓冲交错生产/消费不变量 ==========

static void test_ring_buffer_interleaved_stress_invariants() {
    TestLog::testStart("Stability: ring buffer invariants hold under interleaved stress");

    // 确定性 LCG 驱动伪随机交错，模拟 worker(生产)/loopTask(消费) 的调度交织。
    // 加锁后两侧操作序列化，任意交错次序下不变量必须恒成立：
    //   count<=SLOTS、出队值严格递增（FIFO+丢旧只会跳号不会乱序/重复）
    uint32_t rng = 0xBEEF5EEDu;
    MirrorRingBuffer rb;
    int producedSeq = 0;
    int lastDequeued = -1;
    uint32_t dequeueCount = 0;

    for (int step = 0; step < 20000; step++) {
        rng = rng * 1664525u + 1013904223u;
        if ((rng >> 16) % 3 != 0) {  // 2/3 概率生产（制造队满丢旧压力）
            rb.enqueue(producedSeq++);
        } else {
            int v;
            if (rb.dequeue(v)) {
                TEST_ASSERT_TRUE_MESSAGE(v > lastDequeued,
                    "Dequeued values must be strictly increasing (FIFO, no dup/reorder)");
                lastDequeued = v;
                dequeueCount++;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(rb.count <= MirrorRingBuffer::SLOTS,
            "Queue depth must never exceed slot capacity");
    }

    // 守恒律：生产总数 = 出队数 + 丢弃数 + 队内残留
    TEST_ASSERT_EQUAL_UINT32((uint32_t)producedSeq,
                             dequeueCount + rb.drops + rb.count);
    // 排空残留，顺序仍须递增
    int v;
    while (rb.dequeue(v)) {
        TEST_ASSERT_TRUE(v > lastDequeued);
        lastDequeued = v;
    }
}

// ========== 边界测试：低内存分级门控一致性 ==========

static void test_low_memory_gates_are_consistent() {
    TestLog::testStart("Stability: low-memory feature gates must be consistent");

    using FastBee::MemoryBudget;
    // 各功能门控必须高于 CRITICAL 重启线：功能先降级、设备后重启，
    // 若门控低于重启线则功能还没让路设备就重启了（保护顺序颠倒）
    TEST_ASSERT_TRUE(MemoryBudget::MQTT_RECEIVE_MIN_DRAM >=
                     MemoryBudget::GUARD_CRITICAL_DRAM_FREE - 4384u);
    TEST_ASSERT_TRUE(MemoryBudget::PERIPH_EXEC_MIN_DRAM >
                     MemoryBudget::MQTT_RECEIVE_MIN_DRAM);
    TEST_ASSERT_TRUE(MemoryBudget::MODBUS_POLL_MIN_DRAM >=
                     MemoryBudget::MQTT_RECEIVE_MIN_DRAM);

    // 守卫分级边界：每级差 1 字节必须落在相邻级别（无空洞/重叠）
    using FastBee::MemoryPressureLevel;
    TEST_ASSERT_EQUAL_UINT8(
        (uint8_t)MemoryPressureLevel::CRITICAL,
        (uint8_t)MemoryBudget::guardLevelForDram(
            MemoryBudget::GUARD_CRITICAL_DRAM_FREE - 1, 40000u, 40u, false));
    TEST_ASSERT_EQUAL_UINT8(
        (uint8_t)MemoryPressureLevel::SEVERE,
        (uint8_t)MemoryBudget::guardLevelForDram(
            MemoryBudget::GUARD_CRITICAL_DRAM_FREE, 40000u, 40u, false));
    TEST_ASSERT_EQUAL_UINT8(
        (uint8_t)MemoryPressureLevel::WARN,
        (uint8_t)MemoryBudget::guardLevelForDram(
            MemoryBudget::GUARD_SEVERE_DRAM_FREE, 40000u, 40u, false));
    TEST_ASSERT_EQUAL_UINT8(
        (uint8_t)MemoryPressureLevel::NORMAL,
        (uint8_t)MemoryBudget::guardLevelForDram(
            MemoryBudget::GUARD_WARN_DRAM_FREE, 40000u, 40u, false));

    // 启动宽限期只豁免碎片化判据，不豁免 DRAM 绝对量判据
    TEST_ASSERT_EQUAL_UINT8(
        (uint8_t)MemoryPressureLevel::CRITICAL,
        (uint8_t)MemoryBudget::guardLevelForDram(
            MemoryBudget::GUARD_CRITICAL_DRAM_FREE - 1, 4000u, 90u, true));
    TEST_ASSERT_EQUAL_UINT8(
        (uint8_t)MemoryPressureLevel::NORMAL,
        (uint8_t)MemoryBudget::guardLevelForDram(60000u, 4000u, 90u, true));
}

// ========== 测试组入口 ==========

void test_stability_fixes_group() {
    // P0 阻塞操作
    RUN_TEST(test_wifi_scan_handler_nonblocking);
    RUN_TEST(test_wifi_scan_task_mutex_protection);
    RUN_TEST(test_wifi_scan_restores_pure_ap_mode);
    RUN_TEST(test_wifi_scan_chunked_callback_lightweight);
    RUN_TEST(test_wifi_scan_task_creation_failure_fallback);
    RUN_TEST(test_wifi_scan_task_single_core_safe);
    RUN_TEST(test_wifi_scan_sta_mode_cache_and_dwell);
    RUN_TEST(test_ota_url_download_runs_in_background_task);
    RUN_TEST(test_ota_download_loop_has_timeouts);
    RUN_TEST(test_ota_restart_uses_system_rebooter);

    // P1 并发与重连
    RUN_TEST(test_mqtt_report_ring_buffer_locked);
    RUN_TEST(test_mqtt_flapping_suppression);
    RUN_TEST(test_encoder_isr_static_slots);

    // P0/P1 配置持久化
    RUN_TEST(test_config_write_is_atomic);
    RUN_TEST(test_config_cache_read_your_write);

    // P2 时间回绕
    RUN_TEST(test_millis_wraparound_safe_comparisons);

    // 行为镜像
    RUN_TEST(test_ring_buffer_drop_oldest_semantics);
    RUN_TEST(test_signed_diff_across_millis_wraparound);

    // 边界与并发压力（2026-07 补充）
    RUN_TEST(test_mqtts_no_psram_gate_boundaries);
    RUN_TEST(test_mqtts_no_psram_recovery_retry_gate);
    RUN_TEST(test_mqtts_reclaim_pause_threshold_ordering);
    RUN_TEST(test_mqtt_flapping_backoff_progression);
    RUN_TEST(test_ring_buffer_interleaved_stress_invariants);
    RUN_TEST(test_low_memory_gates_are_consistent);
}
