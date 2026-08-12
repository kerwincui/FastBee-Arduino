#include "./network/handlers/ProvisionRouteHandler.h"
#include "./network/handlers/HandlerUtils.h"
#include "./network/WebHandlerContext.h"
#include "./network/NetworkManager.h"
#include "./systems/LoggerSystem.h"
#include "./core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "./core/PeriphExecManager.h"
#endif
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "./core/ChipConfig.h"
#include "./core/SystemConstants.h"

// 统一使用 device.json 存储配网配置
static const char* DEVICE_CONFIG_FILE = "/config/device.json";

// 清理旧的独立配置文件（已迁移到 device.json）
static void cleanupOldConfigFiles() {
    const char* OLD_PROVISION_CONFIG_FILE = "/config/provision.json";
    const char* OLD_BLE_PROVISION_CONFIG_FILE = "/config/ble_provision.json";
    if (LittleFS.exists(OLD_PROVISION_CONFIG_FILE)) {
        LittleFS.remove(OLD_PROVISION_CONFIG_FILE);
    }
    if (LittleFS.exists(OLD_BLE_PROVISION_CONFIG_FILE)) {
        LittleFS.remove(OLD_BLE_PROVISION_CONFIG_FILE);
    }
}

ProvisionRouteHandler::ProvisionRouteHandler(WebHandlerContext* ctx)
    : ctx(ctx) {
    // 清理旧的配置文件
    cleanupOldConfigFiles();
    // 初始化扫描状态互斥锁（懒加载场景下仅创建一次）
    if (!scanMutex) {
        scanMutex = xSemaphoreCreateMutex();
    }
}

void ProvisionRouteHandler::setupRoutes(AsyncWebServer* server) {
    server->on("/setup", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleSetupPage(request); });

    server->on("/api/wifi/scan", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleWiFiScan(request); });

    server->on("/api/wifi/connect", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleWiFiConnect(request); });

    // AP 配网公开接口（无需身份认证，供手机端 App 直接调用）
    server->on("/api/wifi/status", HTTP_GET,
              [this](AsyncWebServerRequest* request) { handleWiFiStatus(request); });

    server->on("/api/wifi/config", HTTP_POST,
              [this](AsyncWebServerRequest* request) { handleWiFiConfig(request); });

    // 启动时预热扫描缓存：确保首次 /api/wifi/scan 命中缓存。
    // STA 模式下冷扫描会让射频离开当前信道数秒，HTTP 连接因 ACK 超时被
    // AsyncTCP 强制关闭（ERR_CONNECTION_RESET）；开机预热时尚无用户连接，无副作用。
    if (scanMutex && xSemaphoreTake(scanMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool needPrewarm = !scanTaskRunning && !scanResultReady;
        if (needPrewarm) {
            scanTaskRunning = true;
        }
        xSemaphoreGive(scanMutex);
        if (needPrewarm) {
            _launchScanTask();
        }
    }
}

void ProvisionRouteHandler::handleSetupPage(AsyncWebServerRequest* request) {
    ctx->sendBuiltinSetupPage(request);
}

void ProvisionRouteHandler::handleWiFiScan(AsyncWebServerRequest* request) {
    // 非阻塞扫描架构：
    //  - 重活（WiFi.scanNetworks 阻塞扫描 + JSON 构建）运行在独立 FreeRTOS 任务；
    //  - async_tcp 任务上的 chunked 回调仅短暂加锁轮询 scanResultReady 标志并拷贝结果，
    //    绝不在其上调用 WiFi API / 构建 JSON，避免 async_tcp 小栈过载导致连接中断
    //    （ERR_INCOMPLETE_CHUNKED_ENCODING）。
    //  - STA 模式下扫描会导致射频跳频、中断当前 WiFi 连接（ERR_CONNECTION_RESET），
    //    因此使用结果缓存（SCAN_CACHE_TTL_MS）：缓存有效期内直接返回，不触发新扫描。
    if (!scanMutex) {  // 防御：构造函数正常已创建
        scanMutex = xSemaphoreCreateMutex();
    }

    bool needLaunch = false;
    if (xSemaphoreTake(scanMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // 缓存命中：上次扫描结果在 TTL 内，直接复用（避免 STA 模式下重复扫描断连）
        bool cacheFresh = scanResultReady &&
                          (millis() - scanLastCompleteMs < SCAN_CACHE_TTL_MS);
        if (!cacheFresh && !scanTaskRunning) {
            scanTaskRunning = true;   // 置位去重并发请求
            // 注意：不作废 scanResultReady —— 保留旧结果供扫描期间重试请求使用
            needLaunch = true;
        }
        xSemaphoreGive(scanMutex);
    }
    if (needLaunch) {
        _launchScanTask();
    }

    // chunked 响应轮询结果就绪标志；未就绪时返回 TRY_AGAIN 让出 async_tcp
    struct ScanState {
        String json;
        size_t sent = 0;
        unsigned long startMs = 0;
        bool done = false;
    };
    auto state = std::make_shared<ScanState>();
    state->startMs = millis();
    auto* self = this;

    AsyncWebServerResponse* response = request->beginChunkedResponse(
        "application/json",
        [state, self](uint8_t* buffer, size_t maxLen, size_t index) -> size_t {
            (void)index;
            if (!state->done) {
                bool got = false;
                if (self->scanMutex &&
                    xSemaphoreTake(self->scanMutex, 0) == pdTRUE) {
                    if (self->scanResultReady) {
                        state->json = self->scanResultJson;  // 仅拷贝，无重活
                        // 不清除 scanResultReady：缓存模式下结果可被多个请求复用，
                        // 新鲜度由 handleWiFiScan 入口的 TTL 判断控制
                        got = true;
                    }
                    xSemaphoreGive(self->scanMutex);
                }
                if (got) {
                    state->done = true;
                } else if (millis() - state->startMs < 10000UL) {
                    return RESPONSE_TRY_AGAIN;  // 结果未就绪，稍后重试（不阻塞）
                } else {
                    state->json = F("{\"success\":false,\"error\":\"scan_timeout\","
                                    "\"message\":\"WiFi scan timed out, please try again\"}");
                    state->done = true;
                }
            }
            size_t remaining = state->json.length() - state->sent;
            if (remaining == 0) return 0;  // 全部发完，结束 chunked 响应
            size_t chunk = remaining < maxLen ? remaining : maxLen;
            memcpy(buffer, state->json.c_str() + state->sent, chunk);
            state->sent += chunk;
            return chunk;
        });
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
}

void ProvisionRouteHandler::_launchScanTask() {
    // 在独立任务上执行阻塞扫描，栈 6KB 足够 WiFi 扫描 + JSON 构建
    // 单核芯片（C3/C6）绑定 Core 1 会触发 FreeRTOS assert 崩溃，必须按核数区分
    BaseType_t ok =
#if CHIP_DUAL_CORE
        xTaskCreatePinnedToCore(
            _wifiScanTask, "fb_wifi_scan", 6144, this, 1, nullptr, 1);
#else
        xTaskCreate(
            _wifiScanTask, "fb_wifi_scan", 6144, this, 1, nullptr);
#endif
    if (ok != pdPASS) {
        // 创建失败：直接置一个失败结果并清除运行标志，避免后续请求被永久阻塞
        if (scanMutex && xSemaphoreTake(scanMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            scanResultJson = F("{\"success\":false,\"error\":\"scan_failed\","
                               "\"message\":\"WiFi scan failed, please try again\"}");
            scanResultReady = true;
            scanTaskRunning = false;
            xSemaphoreGive(scanMutex);
        } else {
            scanTaskRunning = false;
        }
    }
}

// 将 WiFi 加密枚举映射为前端安全类型字符串（open/wep/wpa/wpa2/wpa3），
// 与 network.html 的 wifi-security 下拉选项取值对齐，供点选网络后自动回填
static const char* _authModeToString(wifi_auth_mode_t auth) {
    switch (auth) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2";
        case WIFI_AUTH_WPA3_PSK:
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa3";
        default:                        return "wpa2";  // 未知类型按最常见处理
    }
}

void ProvisionRouteHandler::_wifiScanTask(void* arg) {
    auto* self = static_cast<ProvisionRouteHandler*>(arg);

    // 记录扫描前模式：scanNetworks 内部会 enableSTA(true) 将纯 AP 切为 APSTA，
    // 扫描结束后需恢复纯 AP，避免 STA 残留触发自动重连/模式干扰
    const bool wasPureAP = (WiFi.getMode() == WIFI_MODE_AP);
    const bool wasSTA = (WiFi.getMode() == WIFI_MODE_STA ||
                           WiFi.getMode() == WIFI_MODE_APSTA);

    // 有界阻塞扫描（在本独立任务上执行，不占用 async_tcp）
    WiFi.setScanTimeout(9000);
    // STA 模式下缩短每信道驻留时间（默认 120ms → 60ms），减少连接中断窗口
    int n;
    if (wasSTA) {
        n = WiFi.scanNetworks(false, false, false, 60);
    } else {
        n = WiFi.scanNetworks();
    }

    String json;
    if (n < 0) {
        // 审计 P2-5：失败/超时路径也必须释放驱动内残留的扫描结果，
        // 否则残留缓存随多次失败扫描累积，且下次 scanNetworks 行为不可预期
        WiFi.scanDelete();
        json = F("{\"success\":false,\"error\":\"scan_failed\","
                 "\"message\":\"WiFi scan failed, please try again\"}");
    } else {
        JsonDocument doc;
        JsonArray networks = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n && i < 20; i++) {
            JsonObject net = networks.add<JsonObject>();
            net["ssid"] = WiFi.SSID(i);
            net["rssi"] = WiFi.RSSI(i);
            net["channel"] = WiFi.channel(i);
            wifi_auth_mode_t auth = WiFi.encryptionType(i);
            net["encrypted"] = (auth != WIFI_AUTH_OPEN);      // setup.html 兼容字段
            net["encryption"] = _authModeToString(auth);      // 主应用安全类型回填
        }
        WiFi.scanDelete();
        doc["success"] = true;
        doc["count"] = n;
        serializeJson(doc, json);
    }

    // 恢复纯 AP 模式（撤销扫描时 enableSTA 引入的 APSTA 切换）
    if (wasPureAP && WiFi.getMode() != WIFI_MODE_AP) {
        WiFi.mode(WIFI_MODE_AP);
    }

    // 结果写入共享缓存并通知 HTTP chunked 回调取走
    if (self->scanMutex &&
        xSemaphoreTake(self->scanMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        self->scanResultJson = json;
        self->scanResultReady = true;
        // 仅扫描成功时刷新 TTL；失败结果立即视为过期，下次请求马上重试
        self->scanLastCompleteMs = (n < 0)
            ? (millis() - SCAN_CACHE_TTL_MS)
            : millis();
        self->scanTaskRunning = false;
        xSemaphoreGive(self->scanMutex);
    } else {
        self->scanTaskRunning = false;  // 兜底清除，避免阻塞后续请求
    }

    vTaskDelete(NULL);
}

void ProvisionRouteHandler::handleWiFiConnect(AsyncWebServerRequest* request) {
    String ssid = ctx->getParamValue(request, "ssid", "");
    String password = ctx->getParamValue(request, "password", "");

    if (ssid.isEmpty()) {
        ctx->sendBadRequest(request, "SSID is required");
        return;
    }

    LOG_INFOF("[Provision] WiFi connect request: SSID=%s", ssid.c_str());

    // 解析可选的扩展配网参数（仅当参数存在时才更新 device.json）
    String userId    = ctx->getParamValue(request, "userId", "");
    String deviceNum = ctx->getParamValue(request, "deviceNum", "");
    String extra     = ctx->getParamValue(request, "extra", "");

    bool hasExtParam = !userId.isEmpty() || !deviceNum.isEmpty() || !extra.isEmpty();
    if (hasExtParam) {
        LOG_INFOF("[Provision] Extended params: userId=%s deviceNum=%s extra=%s",
                  userId.c_str(), deviceNum.c_str(), extra.c_str());
        _updateDeviceConfig(userId, deviceNum, extra);
    }

    // 通过 NetworkManager 保存配置并连接
    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        WiFiConfig cfg = netMgr->getConfig();
        
        // 更新 STA 配置
        cfg.staSSID = ssid;
        cfg.staPassword = password;
        
        // 切换到 STA 模式连接 WiFi
        cfg.mode = NetworkMode::NETWORK_STA;
        
        // 保存配置并触发网络重启
        if (netMgr->updateConfig(cfg, true)) {
            LOG_INFO("[Provision] WiFi configuration saved, network will restart");
            JsonDocument doc;
            doc["success"] = true;
            doc["message"] = "WiFi configuration saved, connecting...";
            doc["data"]["ssid"] = ssid;
            doc["data"]["mode"] = static_cast<uint8_t>(cfg.mode);
            String output;
            serializeJson(doc, output);
            request->send(200, "application/json", output);
            return;
        } else {
            LOG_ERROR("[Provision] Failed to save WiFi configuration");
            ctx->sendError(request, 500, "Failed to save WiFi configuration");
            return;
        }
    }

    // 回退：直接连接（不保存配置）
    ctx->sendSuccess(request, "Connecting to WiFi...");
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
}

// ---------------------------------------------------------------------------
// 内部工具：将配网下发的扩展参数写入 device.json（仅更新提供的字段）
// ---------------------------------------------------------------------------
void ProvisionRouteHandler::_updateDeviceConfig(const String& userId,
                                                 const String& deviceNum,
                                                 const String& extra) {
    JsonDocument doc;

    // 读取现有 device.json
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            deserializeJson(doc, f);
            f.close();
        }
    }

    bool changed = false;

    // userId → device.json.userId
    if (!userId.isEmpty()) {
        doc["userId"] = userId;
        changed = true;
        LOG_INFOF("[Provision] device.json: userId=%s", userId.c_str());
    }

    // deviceNum → device.json.deviceId
    if (!deviceNum.isEmpty()) {
        doc["deviceId"] = deviceNum;
        changed = true;
        LOG_INFOF("[Provision] device.json: deviceId=%s", deviceNum.c_str());
    }

    // extra → device.json.productNumber（仅当 extra 为有效正整数时保存）
    if (!extra.isEmpty()) {
        long pn = extra.toInt();
        if (pn > 0) {
            doc["productNumber"] = (int)pn;
            changed = true;
            LOG_INFOF("[Provision] device.json: productNumber=%d", (int)pn);
        } else {
            LOG_WARNINGF("[Provision] extra='%s' is not a valid positive integer, discarded", extra.c_str());
        }
    }

    if (changed) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "w");
        if (f) {
            serializeJsonPretty(doc, f);
            f.close();
            LOG_INFO("[Provision] device.json updated");
        } else {
            LOG_ERROR("[Provision] Failed to write device.json");
        }
    }
}

// ---------------------------------------------------------------------------
// GET /api/wifi/status — 检测设备状态（公开接口，无需认证）
// ---------------------------------------------------------------------------
void ProvisionRouteHandler::handleWiFiStatus(AsyncWebServerRequest* request) {
    JsonDocument doc;
    doc["success"] = true;
    JsonObject data = doc["data"].to<JsonObject>();

    // ---- 设备基本信息（从 device.json 读取） ----
    String deviceName = "FastBee Device";
    String deviceId;
    String userId;
    if (LittleFS.exists(DEVICE_CONFIG_FILE)) {
        File f = LittleFS.open(DEVICE_CONFIG_FILE, "r");
        if (f) {
            JsonDocument dDoc;
            DeserializationError err = deserializeJson(dDoc, f);
            f.close();
            if (!err) {
                if (dDoc["deviceName"].is<const char*>() &&
                    strlen(dDoc["deviceName"].as<const char*>()) > 0) {
                    deviceName = dDoc["deviceName"].as<String>();
                }
                if (dDoc["deviceId"].is<const char*>())
                    deviceId = dDoc["deviceId"].as<String>();
                if (dDoc["userId"].is<const char*>())
                    userId = dDoc["userId"].as<String>();
            }
        }
    }
    data["deviceName"] = deviceName;
    data["deviceId"] = deviceId;
    data["userId"] = userId;
    data["firmwareVersion"] = SystemInfo::VERSION;

    // ---- 网络状态 ----
    wifi_mode_t mode = WiFi.getMode();
    const char* modeStr = "unknown";
    switch (mode) {
        case WIFI_MODE_AP:    modeStr = "ap";    break;
        case WIFI_MODE_STA:   modeStr = "sta";   break;
        case WIFI_MODE_APSTA: modeStr = "apsta"; break;
        default: break;
    }
    data["networkMode"] = modeStr;

    bool connected = (WiFi.status() == WL_CONNECTED);
    data["connected"] = connected;

    if (connected) {
        data["ipAddress"] = WiFi.localIP().toString();
        data["ssid"] = WiFi.SSID();
        data["rssi"] = WiFi.RSSI();
    } else {
        data["ipAddress"] = (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
                            ? WiFi.softAPIP().toString() : String("");
    }

    data["apSSID"] = WiFi.softAPSSID();
    data["apIP"] = WiFi.softAPIP().toString();
    data["macAddress"] = WiFi.macAddress();

    // ---- 芯片 / 内存信息 ----
    data["chipModel"] = ESP.getChipModel();
    data["chipRevision"] = ESP.getChipRevision();
    data["freeHeap"] = ESP.getFreeHeap();

    String output;
    serializeJson(doc, output);
    request->send(200, "application/json", output);
}

// ---------------------------------------------------------------------------
// POST /api/wifi/config — 配网下发配置（公开接口，无需认证）
// ---------------------------------------------------------------------------
void ProvisionRouteHandler::handleWiFiConfig(AsyncWebServerRequest* request) {
    // 必须参数：SSID/ssid, password, userId
    // 可选参数：deviceNum, extra
    String ssid     = ctx->getParamValue(request, "SSID", "");
    String password = ctx->getParamValue(request, "password", "");
    String userId   = ctx->getParamValue(request, "userId", "");

    // 兼容小写 ssid 和 pwd 别名
    if (ssid.isEmpty())     ssid     = ctx->getParamValue(request, "ssid", "");
    if (password.isEmpty()) password = ctx->getParamValue(request, "pwd", "");

    if (ssid.isEmpty() || password.isEmpty() || userId.isEmpty()) {
        JsonDocument errDoc;
        errDoc["success"] = false;
        errDoc["message"] = "配网必须传递 SSID、WiFi密码和用户编号(userId)";
        String errOutput;
        serializeJson(errDoc, errOutput);
        request->send(400, "application/json", errOutput);
        return;
    }

    LOG_INFOF("[Provision] WiFi config: SSID=%s userId=%s", ssid.c_str(), userId.c_str());

    // 解析可选参数
    String deviceNum = ctx->getParamValue(request, "deviceNum", "");
    String extra     = ctx->getParamValue(request, "extra", "");

    bool hasExtParam = !userId.isEmpty() || !deviceNum.isEmpty() || !extra.isEmpty();
    if (hasExtParam) {
        LOG_INFOF("[Provision] Extended params: userId=%s deviceNum=%s extra=%s",
                  userId.c_str(), deviceNum.c_str(), extra.c_str());
    }

    // 1. 保存设备信息到 device.json
    _updateDeviceConfig(userId, deviceNum, extra);

    // 2. 保存 WiFi 配置到 network.json 并切换到 STA 模式
    if (ctx->networkManager) {
        FBNetworkManager* netMgr = static_cast<FBNetworkManager*>(ctx->networkManager);
        WiFiConfig cfg = netMgr->getConfig();

        cfg.staSSID = ssid;
        cfg.staPassword = password;
        cfg.mode = NetworkMode::NETWORK_STA;

        if (netMgr->updateConfig(cfg, true)) {
            LOG_INFO("[Provision] WiFi config saved, network will restart to STA");
            JsonDocument doc;
            doc["success"] = true;
            doc["message"] = "设备已更新WiFi配置，正在连接...";
            doc["data"]["ssid"] = ssid;
            doc["data"]["mode"] = static_cast<uint8_t>(cfg.mode);
            String output;
            serializeJson(doc, output);
            request->send(200, "application/json", output);
            return;
        } else {
            LOG_ERROR("[Provision] Failed to save WiFi configuration");
            ctx->sendError(request, 500, "Failed to save WiFi configuration");
            return;
        }
    }

    // 回退：无 NetworkManager 时直接连接（不保存配置）
    LOG_WARNING("[Provision] No NetworkManager, falling back to direct WiFi.begin");
    ctx->sendSuccess(request, "Connecting to WiFi...");
    delay(100);
    WiFi.begin(ssid.c_str(), password.c_str());
}
