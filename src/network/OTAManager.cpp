/**
 * @description: OTA管理器，负责固件更新功能
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:30:15
 */

#include "network/OTAManager.h"
#include "systems/LoggerSystem.h"
#include "systems/RestartDiagnostics.h"
#include "systems/SystemRebooter.h"
#include "core/FeatureFlags.h"
#include "core/FastBeeFramework.h"
#include "network/NetworkManager.h"
#include "protocols/FastBeeOTAProtocol.h"
#if FASTBEE_ENABLE_HEALTH_MONITOR
#include "systems/HealthMonitor.h"
#endif
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <new>
#include <memory>
#include <time.h>

#if FASTBEE_ENABLE_OTA

// OTA状态枚举
enum OTAStatus {
    OTA_IDLE = 0,
    OTA_DOWNLOADING = 1,
    OTA_UPLOADING = 2,
    OTA_COMPLETED = 3,
    OTA_FAILED = 4
};

static OTAStatus otaStatus = OTA_IDLE;
static String otaErrorMessage = "";
static String otaFirmwareUrl = "";
static int otaTaskId = 0;

// URL OTA 后台任务防重入门闩（与 otaInProgress 共同构成双重保护）
static portMUX_TYPE s_otaSpawnMux = portMUX_INITIALIZER_UNLOCKED;
static bool s_otaUrlTaskActive = false;
static bool s_otaSessionStarting = false;

namespace {
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

void sendOtaJson(AsyncWebServerRequest* request, int status, JsonDocument& doc) {
    String body;
    serializeJson(doc, body);
    request->send(status, "application/json", body);
}

struct OtaUrlTaskParams {
    OTAManager* manager;
    String url;
    OTAManager::ProgressCallback progressCallback;
    OTAManager::CompletionCallback completionCallback;
    bool rebootOnSuccess;
};

// 独立低优先级任务执行下载+写 flash，避免在 async_tcp 上同步阻塞分钟级
// （下载数据本身也走 async_tcp 收包，同任务执行存在自饿死风险）
void otaUrlDownloadTask(void* pv) {
    OtaUrlTaskParams* params = static_cast<OtaUrlTaskParams*>(pv);
    const bool success = params->manager->startOTA(
        params->url, params->progressCallback, params->rebootOnSuccess);
    const String error = success ? String() : params->manager->getErrorMessage();

    // startOTA 已完全退出并释放 OTA 会话。先释放后台任务门闩，再通知终态，
    // 保证完成回调触发的新任务不会被已经结束的任务误判为 busy。
    portENTER_CRITICAL(&s_otaSpawnMux);
    s_otaUrlTaskActive = false;
    portEXIT_CRITICAL(&s_otaSpawnMux);

    if (params->completionCallback) {
        params->completionCallback(success, error);
    }
    delete params;
    vTaskDelete(nullptr);
}
}  // namespace

// 构造函数
OTAManager::OTAManager(AsyncWebServer* webServer)
    : otaInProgress(false),
      otaUploadMode(false),
      otaStartTime(0),
      otaLastChunkTime(0),
      totalSize(0),
      totalExpected(0),
      currentSize(0),
      progress(0) {
    (void)webServer;  // 路由已移交 OTARouteHandler 统一注册，此参数保留兼容性
}

// 初始化（路由由 OTARouteHandler 注册并委托到本类，此处仅重置状态）
bool OTAManager::initialize() {
    otaInProgress = false;
    otaUploadMode = false;
    otaStatus = OTA_IDLE;
    LOG_INFO("OTAManager: Initialized successfully");
    return true;
}

// 计算当前可用的最大 Sketch 升级空间
size_t OTAManager::calcMaxSketchSpace() {
    return (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
}

// 内存门控：OTA 写入需要 ~4KB 临时缓冲，SEVERE/CRITICAL 状态或 DRAM 不足时拒绝
bool OTAManager::checkMemoryGate(String* reason) {
    uint32_t dramFree = heap_caps_get_free_size(MALLOC_CAP_8BIT);
#if FASTBEE_ENABLE_HEALTH_MONITOR
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    HealthMonitor* monitor = fw ? fw->getHealthMonitor() : nullptr;
    if (monitor && monitor->getMemoryGuardLevel() >= MemoryGuardLevel::SEVERE) {
        if (reason) *reason = "Memory pressure too high for OTA, retry later";
        return false;
    }
#endif
    // 硬门槛：至少 20KB 8BIT DRAM 可用
    if (dramFree < OTA_MIN_FREE_DRAM) {
        if (reason) *reason = "Insufficient memory for OTA update (DRAM=" + String(dramFree) + "B)";
        return false;
    }
    return true;
}

// 开始一次 OTA 写入会话（expectedSize=0 表示大小未知，按 maxSketchSpace 预留）
bool OTAManager::beginOTA(size_t expectedSize) {
    // 惰性回收僵死会话，避免上次异常断连永久阻塞新的升级请求
    checkStalledUpload();
    portENTER_CRITICAL(&s_otaSpawnMux);
    const bool busy = otaInProgress || s_otaSessionStarting;
    if (!busy) s_otaSessionStarting = true;
    portEXIT_CRITICAL(&s_otaSpawnMux);
    if (busy) {
        otaErrorMessage = "OTA already in progress";
        LOG_WARNING("OTAManager: " + otaErrorMessage);
        return false;
    }

    auto releaseStartClaim = []() {
        portENTER_CRITICAL(&s_otaSpawnMux);
        s_otaSessionStarting = false;
        portEXIT_CRITICAL(&s_otaSpawnMux);
    };

    // OTA 分片预检：4MB 等单应用分片表没有 ota_1，无法双分片升级，提前返回明确错误
    // （esp_ota_get_next_update_partition 在单应用布局会回绕返回运行中的 app0，不可用于判断）
    const esp_partition_t* otaPartition =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
    if (!otaPartition) {
        otaErrorMessage = "No OTA partition available (single-app flash layout)";
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        releaseStartClaim();
        return false;
    }

    // 内存门控（DRAM≥20KB 硬门槛 + HealthMonitor SEVERE 检查）
    String reason;
    if (!checkMemoryGate(&reason)) {
        otaErrorMessage = reason;
        LOG_WARNING("OTAManager: OTA rejected - " + reason);
        releaseStartClaim();
        return false;
    }

    size_t maxSketchSpace = calcMaxSketchSpace();
    if (expectedSize > 0 && expectedSize > maxSketchSpace) {
        otaErrorMessage = "Firmware too large: " + String(expectedSize) + " > " + String(maxSketchSpace);
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        releaseStartClaim();
        return false;
    }

    // 大小未知时按最大 Sketch 空间预留，实际写入量以 endOTA 校验为准
    if (!Update.begin(expectedSize == 0 ? maxSketchSpace : expectedSize)) {
        otaErrorMessage = "Failed to start OTA: " + String(Update.errorString());
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        releaseStartClaim();
        return false;
    }

    otaUploadMode = true;
    otaStartTime = millis();
    otaLastChunkTime = millis();
    totalExpected = expectedSize;
    totalSize = expectedSize;
    currentSize = 0;
    progress = 0;
    otaStatus = OTA_UPLOADING;
    otaErrorMessage = "";
    portENTER_CRITICAL(&s_otaSpawnMux);
    otaInProgress = true;
    s_otaSessionStarting = false;
    portEXIT_CRITICAL(&s_otaSpawnMux);
    LOG_INFO("OTAManager: OTA session started, expected=" + String(expectedSize) + " bytes");
    return true;
}

// 写入一个固件数据分块，失败返回 0 并终止会话
size_t OTAManager::writeData(const uint8_t* data, size_t len) {
    if (!otaInProgress) {
        otaErrorMessage = "OTA not started";
        return 0;
    }

    // 每个分块到达即刷新停滞计时（写入失败路径会直接终止会话）
    otaLastChunkTime = millis();

    // Update.write 签名为非 const 但内部不修改数据，此处安全去 const
    if (Update.write(const_cast<uint8_t*>(data), len) != len) {
        otaErrorMessage = "Write failed: " + String(Update.errorString());
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        Update.end(false);
        otaInProgress = false;
        otaStatus = OTA_FAILED;
        return 0;
    }

    currentSize += len;
    if (totalExpected > 0) {
        progress = (uint8_t)((currentSize * 100) / totalExpected);
    }
    return len;
}

// 结束 OTA 写入并校验固件完整性
bool OTAManager::endOTA() {
    if (!otaInProgress) {
        otaErrorMessage = "OTA not started";
        return false;
    }
    otaUploadMode = false;  // 进入验证/重启阶段，不再受停滞检查影响

    if (totalExpected > 0 && currentSize != totalExpected) {
        otaErrorMessage = "Firmware size mismatch: expected " + String(totalExpected) +
                          ", received " + String(currentSize);
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        Update.end(false);
        otaInProgress = false;
        otaStatus = OTA_FAILED;
        return false;
    }

    // 只有 Web 上传未提供大小时才允许 Update 按实际写入长度结束。已知大小
    // 必须走严格结束，不能用 evenIfRemaining=true 掩盖不完整下载。
    const bool allowUnknownSize = totalExpected == 0;
    if (!Update.end(allowUnknownSize)) {
        otaErrorMessage = "OTA failed: " + String(Update.errorString());
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaInProgress = false;
        otaStatus = OTA_FAILED;
        return false;
    }

    if (!Update.isFinished()) {
        otaErrorMessage = "Firmware verification failed";
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaInProgress = false;
        otaStatus = OTA_FAILED;
        return false;
    }

    otaInProgress = false;
    otaStatus = OTA_COMPLETED;
    progress = 100;
    LOG_INFO("OTAManager: OTA update completed, size=" + String(currentSize) + " bytes");
    return true;
}

// 取消当前 OTA 会话（丢弃已写入数据）
void OTAManager::cancelOTA() {
    if (!otaInProgress) return;
    Update.end(false);
    otaInProgress = false;
    otaUploadMode = false;
    otaStatus = OTA_FAILED;
    otaErrorMessage = "OTA cancelled by user";
    LOG_WARNING("OTAManager: OTA update cancelled by user");
}

// 状态 JSON（扁平结构，供测试与脚本使用）
String OTAManager::getStatusJSON() const {
    JsonDocument doc;
    doc["inProgress"] = otaInProgress;
    doc["progress"] = progress;
    doc["currentSize"] = (unsigned long)currentSize;
    doc["totalSize"] = (unsigned long)totalExpected;
    doc["elapsedMs"] = (unsigned long)(millis() - otaStartTime);
    doc["error"] = otaErrorMessage;
    String json;
    serializeJson(doc, json);
    return json;
}

// 回收僵死的上传型 OTA：客户端中途断连后 ESPAsyncWebServer 不再有回调（无 final=true），
// otaInProgress 与半开 Update 状态残留会导致后续所有 /api/ota/begin 返回 400，只能重启设备。
// 与 URL 下载路径的 15s 停滞超时对等保护：60s 无新分块即判停滞并清理
void OTAManager::checkStalledUpload() {
    static const unsigned long OTA_UPLOAD_STALL_MS = 60000;
    if (!otaInProgress || !otaUploadMode) return;
    if (millis() - otaLastChunkTime < OTA_UPLOAD_STALL_MS) return;

    LOG_WARNING("OTAManager: Upload stalled for >60s with no chunk, aborting stale OTA session");
    Update.end(false);
    otaInProgress = false;
    otaUploadMode = false;
    otaStatus = OTA_FAILED;
    otaErrorMessage = "Upload stalled: no chunk for 60s, session auto-recovered";
}

// 处理OTA开始请求
void OTAManager::handleOTAStart(AsyncWebServerRequest *request) {
    // 固件大小参数（可选；缺省/非正数按大小未知处理）
    size_t expectedSize = 0;
    if (request->hasParam("size", true)) {
        long parsed = request->getParam("size", true)->value().toInt();
        if (parsed > 0) expectedSize = (size_t)parsed;
    }

    if (!beginOTA(expectedSize)) {
        // 内存门控拒绝返回 503（可重试），分片表不支持返回 501（永久限制），会话冲突返回 400，其余 500
        int code = 500;
        if (otaErrorMessage.indexOf("already in progress") >= 0) {
            code = 400;
        } else if (otaErrorMessage.indexOf("Insufficient memory") >= 0 ||
                   otaErrorMessage.indexOf("Memory pressure") >= 0) {
            code = 503;
        } else if (otaErrorMessage.indexOf("No OTA partition") >= 0) {
            code = 501;
        }
        JsonDocument errorDoc;
        errorDoc["success"] = false;
        errorDoc["error"] = otaErrorMessage;
        sendOtaJson(request, code, errorDoc);
        return;
    }

    size_t maxSketchSpace = calcMaxSketchSpace();
    LOG_INFO("OTAManager: OTA update started successfully");
    JsonDocument startedDoc;
    startedDoc["success"] = true;
    startedDoc["status"] = "OTA started";
    startedDoc["max_size"] = (unsigned long)maxSketchSpace;
    sendOtaJson(request, 200, startedDoc);
    // 触发OTA升级开始系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
    PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_OTA_START, "");
#endif
}

// 处理OTA上传
void OTAManager::handleOTAUpload(AsyncWebServerRequest *request, const String& filename,
                                 size_t index, uint8_t *data, size_t len, bool final) {
    if (!otaInProgress) {
        LOG_ERROR("OTAManager: OTA not started");
        request->send(400, "application/json", "{\"error\": \"OTA not started\"}");
        return;
    }

    // 文件开始上传
    if (index == 0) {
        LOG_INFO("OTAManager: OTA Update file: " + filename);
    }

    // 写入数据（失败时 writeData 已终止会话并记录错误）
    if (writeData(data, len) != len) {
        JsonDocument errorDoc;
        errorDoc["success"] = false;
        errorDoc["error"] = otaErrorMessage;
        sendOtaJson(request, 500, errorDoc);
        // 触发OTA升级失败系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_OTA_FAILED, otaErrorMessage);
#endif
        return;
    }

    // 文件上传完成
    if (final) {
        LOG_INFO("OTAManager: Upload completed, total size: " + String(currentSize / 1024) + "KB");

        if (endOTA()) {
            LOG_INFO("OTAManager: OTA update completed successfully");

            // 计算MD5校验（如果有）
            String md5 = Update.md5String();
            if (!md5.isEmpty()) {
                LOG_INFO("OTAManager: Firmware MD5: " + md5);
            }

            LOG_INFO("OTAManager: Firmware verification passed");
            // 触发OTA升级成功系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_OTA_SUCCESS, "");
#endif
            JsonDocument completedDoc;
            completedDoc["success"] = true;
            completedDoc["status"] = "OTA completed";
            completedDoc["size"] = (unsigned long)currentSize;
            completedDoc["md5"] = md5;
            completedDoc["message"] = "Restarting in 3 seconds...";
            sendOtaJson(request, 200, completedDoc);

            // 延迟重启交给主循环 SystemRebooter 执行，避免在 async_tcp 任务上
            // 同步睡眠 3 秒导致响应无法 flush 给客户端（发送依赖 async_tcp 泵动）
            RestartDiagnostics::savePreRestartState(
                RestartReason::OTA_UPDATE,
                "OTA firmware upload completed");
            LOG_INFO("OTAManager: Restart scheduled in 3s...");
            SystemRebooter::scheduleReboot("OTA firmware upload completed", 3000,
                                           RestartReason::OTA_UPDATE);
        } else {
            // 触发OTA升级失败系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_OTA_FAILED, otaErrorMessage);
#endif
            JsonDocument errorDoc;
            errorDoc["success"] = false;
            errorDoc["error"] = otaErrorMessage;
            sendOtaJson(request, 500, errorDoc);
        }
    }
    // 注：非 final 分块不得对同一 request 多次 send（ESPAsyncWebServer 协议违规，
    // 会导致重复响应/use-after-free），进度由 /api/ota/status 轮询获取
}

// 处理OTA状态查询
void OTAManager::handleOTAStatus(AsyncWebServerRequest *request) {
    // 状态轮询时同步检查停滞，前端轮询即可触发自愈回收
    checkStalledUpload();
    JsonDocument doc;  // ArduinoJson 7.x 弹性分配，OTA 状态响应 <256B

    doc["in_progress"] = otaInProgress;
    doc["start_time"] = otaStartTime;
    doc["elapsed_ms"] = millis() - otaStartTime;
    doc["current_size"] = currentSize;
    doc["total_size"] = totalExpected;
    doc["remaining_size"] = totalSize;
    doc["progress"] = progress;

    if (otaInProgress) {
        doc["error"] = Update.errorString();
    }

    String response;
    serializeJson(doc, response);
    request->send(200, "application/json", response);
}

// 处理OTA取消请求
void OTAManager::handleOTACancel(AsyncWebServerRequest *request) {
    if (!otaInProgress) {
        JsonDocument idleDoc;
        idleDoc["success"] = true;
        idleDoc["status"] = "No OTA in progress";
        sendOtaJson(request, 200, idleDoc);
        return;
    }

    cancelOTA();
    JsonDocument cancelledDoc;
    cancelledDoc["success"] = true;
    cancelledDoc["status"] = "OTA cancelled";
    sendOtaJson(request, 200, cancelledDoc);
}

// 通过URL进行OTA更新
bool OTAManager::startOTA(const String& url,
                          ProgressCallback progressCallback,
                          bool rebootOnSuccess) {
    if (!FastBeeOtaProtocol::isValidFirmwareUrl(url)) {
        otaErrorMessage = "OTA URL must be a valid HTTP/HTTPS address";
        otaStatus = OTA_FAILED;
        return false;
    }
    const bool useTls = FastBeeOtaProtocol::isHttpsUrl(url);
    // URL OTA 支持 WiFi 和以太网当前活动链路。4G 需要独立 TLS Client；
    // 现有 CellularAdapter 的安全 Client 同时被 MQTT 持有，不能并发复用。
    FastBeeFramework* framework = FastBeeFramework::getInstance();
    FBNetworkManager* networkManager = framework
        ? static_cast<FBNetworkManager*>(framework->getNetworkManager())
        : nullptr;
    const bool networkConnected = networkManager
        ? networkManager->isNetworkConnected()
        : WiFi.status() == WL_CONNECTED;
    if (!networkConnected) {
        otaErrorMessage = "No network connection, cannot download firmware";
        LOG_WARNING("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        return false;
    }
#if FASTBEE_ENABLE_CELLULAR
    if (networkManager && networkManager->getNetworkType() == NetworkType::NET_4G) {
        otaErrorMessage = "URL OTA over cellular requires a dedicated transport client";
        LOG_WARNING("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        return false;
    }
#endif

    // Check admission before allocating a client/starting a TLS handshake.
    // beginOTA also rechecks after connection establishment before flash writes.
    if (!checkMemoryGate(&otaErrorMessage)) {
        otaStatus = OTA_FAILED;
        return false;
    }

    LOG_INFO("OTAManager: Starting OTA from URL: " + url);
    otaFirmwareUrl = url;

    // Construct only the selected client. Plain HTTP needs neither TLS buffers
    // nor a synchronized clock; HTTPS never falls back to an insecure client.
    std::unique_ptr<NetworkClient> client;
    if (useTls) {
        if (time(nullptr) < 1577836800) {  // 2020-01-01 UTC
            otaErrorMessage = "System time is not synchronized for TLS validation";
            otaStatus = OTA_FAILED;
            return false;
        }
        const size_t caBundleSize = static_cast<size_t>(x509_crt_bundle_end - x509_crt_bundle_start);
        if (caBundleSize == 0) {
            otaErrorMessage = "Built-in CA certificate bundle is unavailable";
            otaStatus = OTA_FAILED;
            return false;
        }
        auto secureClient = new (std::nothrow) WiFiClientSecure;
        if (secureClient) {
            secureClient->setCACertBundle(x509_crt_bundle_start, caBundleSize);
            secureClient->setHandshakeTimeout(30);
        }
        client.reset(secureClient);
    } else {
        client.reset(new (std::nothrow) NetworkClient);
    }
    if (!client) {
        otaErrorMessage = "Insufficient memory for OTA download client";
        otaStatus = OTA_FAILED;
        return false;
    }
    HTTPClient http;

    if (!http.begin(*client, url)) {
        otaErrorMessage = "Failed to initialize OTA HTTP/HTTPS client";
        otaStatus = OTA_FAILED;
        return false;
    }
    // HTTPClient::setURL accepts only the current protocol on redirects.
    // In particular an HTTPS endpoint cannot redirect this session to HTTP.
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(30000);  // 30秒超时

    int httpCode = http.GET();

    if (httpCode != HTTP_CODE_OK) {
        otaErrorMessage = "HTTP error: " + String(httpCode);
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        http.end();
        return false;
    }

    long contentLength = http.getSize();
    if (contentLength <= 0) {
        otaErrorMessage = "Firmware response must include a positive Content-Length";
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        http.end();
        return false;
    }
    const size_t expectedSize = static_cast<size_t>(contentLength);
    LOG_INFO("OTAManager: Firmware size: " + String(expectedSize) + " bytes");

    // beginOTA 内部完成停滞回收/并发保护/内存门控/Update.begin
    if (!beginOTA(expectedSize)) {
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        http.end();
        return false;
    }
    otaUploadMode = false;  // URL 下载路径自带停滞超时，不受上传停滞检查影响
    otaStatus = OTA_DOWNLOADING;
    if (progressCallback) progressCallback(0);

    // 获取流并写入
    NetworkClient* stream = http.getStreamPtr();
    uint8_t buff[1024];
    size_t remaining = expectedSize;
    uint8_t lastReportedProgress = 0;

    // 双重超时保护：服务器建连后停发数据时 http.connected() 保持 true，
    // 无超时则此循环永久自旋直至 TWDT panic
    const unsigned long STALL_TIMEOUT_MS = 15000;   // 15s 无新字节判定停滞
    const unsigned long TOTAL_TIMEOUT_MS = 600000;  // 整体 10 分钟 deadline
    unsigned long lastDataMs = millis();

    while (remaining > 0) {
        size_t available = stream->available();
        if (available) {
            size_t readLength = available;
            if (readLength > sizeof(buff)) readLength = sizeof(buff);
            if (readLength > remaining) readLength = remaining;
            int c = stream->readBytes(buff, readLength);
            if (c <= 0) {
                otaErrorMessage = "Firmware stream read failed before completion";
                LOG_ERROR("OTAManager: " + otaErrorMessage);
                Update.end(false);
                otaStatus = OTA_FAILED;
                otaInProgress = false;
                http.end();
                return false;
            }
            if (writeData(buff, static_cast<size_t>(c)) != static_cast<size_t>(c)) {
                // writeData 失败时已终止会话并记录错误
                LOG_ERROR("OTAManager: " + otaErrorMessage);
                otaStatus = OTA_FAILED;
                http.end();
                return false;
            }

            remaining -= static_cast<size_t>(c);
            if (progressCallback &&
                (progress == 100 || progress >= lastReportedProgress + 5U)) {
                lastReportedProgress = progress;
                progressCallback(progress);
            }
            lastDataMs = millis();
        } else if (!http.connected()) {
            break;
        } else if (millis() - lastDataMs > STALL_TIMEOUT_MS) {
            otaErrorMessage = "Download stalled: no data for 15s";
            LOG_ERROR("OTAManager: " + otaErrorMessage);
            Update.end(false);
            otaStatus = OTA_FAILED;
            otaInProgress = false;
            http.end();
            return false;
        }
        if (millis() - otaStartTime > TOTAL_TIMEOUT_MS) {
            otaErrorMessage = "Download timeout: exceeded 10 minutes";
            LOG_ERROR("OTAManager: " + otaErrorMessage);
            Update.end(false);
            otaStatus = OTA_FAILED;
            otaInProgress = false;
            http.end();
            return false;
        }
        delay(1);
    }

    http.end();

    if (remaining != 0 || currentSize != expectedSize) {
        otaErrorMessage = "Firmware download ended early: expected " + String(expectedSize) +
                          ", received " + String(currentSize);
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        Update.end(false);
        otaStatus = OTA_FAILED;
        otaInProgress = false;
        return false;
    }

    if (endOTA()) {
        if (rebootOnSuccess) {
            LOG_INFO("OTAManager: URL OTA completed, restart scheduled in 3s");
            SystemRebooter::scheduleReboot(
                "OTA URL update completed", 3000, RestartReason::OTA_UPDATE);
        }
        return true;
    }

    LOG_ERROR("OTAManager: " + otaErrorMessage);
    return false;
}

// 在独立后台任务中执行URL OTA（供 Web handler 调用，立即返回不阻塞）
bool OTAManager::startOTAAsync(const String& url,
                               ProgressCallback progressCallback,
                               CompletionCallback completionCallback,
                               bool rebootOnSuccess) {
    portENTER_CRITICAL(&s_otaSpawnMux);
    bool busy = otaInProgress || s_otaSessionStarting || s_otaUrlTaskActive;
    if (!busy) s_otaUrlTaskActive = true;
    portEXIT_CRITICAL(&s_otaSpawnMux);
    if (busy) {
        LOG_WARNING("OTAManager: OTA already in progress, async start rejected");
        return false;
    }

    OtaUrlTaskParams* params = new (std::nothrow) OtaUrlTaskParams{
        this, url, progressCallback, completionCallback, rebootOnSuccess};
    if (!params) {
        portENTER_CRITICAL(&s_otaSpawnMux);
        s_otaUrlTaskActive = false;
        portEXIT_CRITICAL(&s_otaSpawnMux);
        otaErrorMessage = "Failed to allocate OTA task parameters";
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        return false;
    }
    // 栈 8KB：HTTPClient + TLS 下载 + Update 写入；优先级 1 低于网络任务
    BaseType_t ok = xTaskCreate(otaUrlDownloadTask, "ota_url", 8192, params, 1, nullptr);
    if (ok != pdPASS) {
        delete params;
        portENTER_CRITICAL(&s_otaSpawnMux);
        s_otaUrlTaskActive = false;
        portEXIT_CRITICAL(&s_otaSpawnMux);
        LOG_ERROR("OTAManager: Failed to create OTA download task");
        return false;
    }
    return true;
}

// 获取OTA状态字符串
String OTAManager::getOTAStatus() {
    if (otaInProgress) {
        if (totalExpected > 0) {
            return "OTA in progress, " + String(progress) + "% complete (" +
                   String(currentSize / 1024) + "KB/" + String(totalExpected / 1024) + "KB)";
        }
        // 未知大小：仅展示已写入量
        return "OTA in progress, " + String(currentSize / 1024) + "KB written (size unknown)";
    } else {
        return "OTA ready";
    }
}

// 获取当前进度
uint8_t OTAManager::getProgress() const {
    return progress;
}

// 检查是否正在进行OTA
bool OTAManager::isOTAInProgress() const {
    return otaInProgress;
}

// 获取已上传大小
size_t OTAManager::getCurrentSize() const {
    return currentSize;
}

// 获取总大小
size_t OTAManager::getTotalSize() const {
    return totalExpected;
}

// 获取已用时间
unsigned long OTAManager::getElapsedTime() const {
    return millis() - otaStartTime;
}

// 获取错误信息
String OTAManager::getErrorMessage() const {
    return otaErrorMessage;
}

#endif // FASTBEE_ENABLE_OTA
