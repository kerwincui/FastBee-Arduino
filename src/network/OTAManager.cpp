/**
 * @description: OTA管理器，负责固件更新功能
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 * @date: 2025-12-02 17:30:15
 */

#include "Network/OTAManager.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_PERIPH_EXEC
#include "core/PeriphExecManager.h"
#endif
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>
#include <HTTPClient.h>
#include <WiFi.h>

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

// 构造函数
OTAManager::OTAManager(AsyncWebServer* webServer) 
    : server(webServer), 
      otaInProgress(false), 
      otaStartTime(0),
      totalSize(0),
      currentSize(0),
      progress(0) {
}

// 初始化
bool OTAManager::initialize() {
    if (!server) {
        LOG_ERROR("OTAManager: Web server is not initialized");
        return false;
    }
    
    setOTACallbacks();
    LOG_INFO("OTAManager: Initialized successfully");
    return true;
}

// 设置OTA回调
void OTAManager::setOTACallbacks() {
    server->on("/api/ota/begin", HTTP_POST, [this](AsyncWebServerRequest *request) { 
        handleOTAStart(request); 
    });
    
    server->on("/api/ota/upload", HTTP_POST, 
        [this](AsyncWebServerRequest *request) {
            // 预处理函数
            if (otaInProgress) {
                request->send(400, "application/json", "{\"error\": \"OTA already in progress\"}");
                return;
            }
        },
        [this](AsyncWebServerRequest *request, const String& filename, size_t index, uint8_t *data, size_t len, bool final) {
            handleOTAUpload(request, filename, index, data, len, final);
        }
    );
    
    server->on("/api/ota/status", HTTP_GET, [this](AsyncWebServerRequest *request) { 
        handleOTAStatus(request); 
    });
    
    server->on("/api/ota/cancel", HTTP_POST, [this](AsyncWebServerRequest *request) { 
        handleOTACancel(request); 
    });
}

// 处理OTA开始请求
void OTAManager::handleOTAStart(AsyncWebServerRequest *request) {
    if (otaInProgress) {
        LOG_WARNING("OTAManager: OTA already in progress");
        request->send(400, "application/json", "{\"error\": \"OTA already in progress\"}");
        return;
    }
    
    // 检查是否有文件大小参数
    if (request->hasParam("size", true)) {
        totalSize = request->getParam("size", true)->value().toInt();
    } else {
        totalSize = 0; // 未知大小
    }
    
    LOG_INFO("OTAManager: Starting OTA update...");
    
    // 设置更新回调
    Update.onProgress([this](size_t progress, size_t total) {
        this->currentSize = progress;
        this->totalSize = total;
        this->progress = total > 0 ? (progress * 100 / total) : 0;
        
        // 每5%进度或至少1MB时记录一次日志
        static int lastReportedProgress = -5;
        if (progress == total || this->progress - lastReportedProgress >= 5) {
            LOG_INFO("OTAManager: Progress: " + String(this->progress) + "%, " + 
                    String(progress / 1024) + "KB/" + String(total / 1024) + "KB");
            lastReportedProgress = this->progress;
        }
    });
    
    // 开始OTA更新
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(totalSize == 0 ? maxSketchSpace : totalSize)) {
        String errorMsg = "Failed to start OTA: " + String(Update.errorString());
        LOG_ERROR("OTAManager: " + errorMsg);
        request->send(500, "application/json", "{\"error\": \"" + errorMsg + "\"}");
        return;
    }
    
    otaInProgress = true;
    otaStartTime = millis();
    currentSize = 0;
    progress = 0;
    
    LOG_INFO("OTAManager: OTA update started successfully");
    request->send(200, "application/json", "{\"status\": \"OTA started\", \"max_size\": " + String(maxSketchSpace) + "}");
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
    
    // 写入数据
    if (Update.write(data, len) != len) {
        String errorMsg = "Write failed: " + String(Update.errorString());
        LOG_ERROR("OTAManager: " + errorMsg);
        Update.end(false);
        otaInProgress = false;
        request->send(500, "application/json", "{\"error\": \"" + errorMsg + "\"}");
        return;
    }
    
    currentSize += len;
    
    // 文件上传完成
    if (final) {
        LOG_INFO("OTAManager: Upload completed, total size: " + String(currentSize / 1024) + "KB");
        
        if (Update.end(true)) {
            LOG_INFO("OTAManager: OTA update completed successfully");
            
            // 计算MD5校验（如果有）
            String md5 = Update.md5String();
            if (!md5.isEmpty()) {
                LOG_INFO("OTAManager: Firmware MD5: " + md5);
            }
            
            // 验证固件
            if (Update.isFinished()) {
                LOG_INFO("OTAManager: Firmware verification passed");
                // 触发OTA升级成功系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
                PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_OTA_SUCCESS, "");
#endif
                request->send(200, "application/json", 
                    "{\"status\": \"OTA completed\", \"size\": " + String(currentSize) + 
                    ", \"md5\": \"" + md5 + "\", \"message\": \"Restarting in 3 seconds...\"}");
                
                // 延迟重启，让客户端有时间收到响应
                delay(3000);
                LOG_INFO("OTAManager: Restarting device...");
                ESP.restart();
            } else {
                LOG_ERROR("OTAManager: Firmware verification failed");
                request->send(500, "application/json", "{\"error\": \"Firmware verification failed\"}");
                otaInProgress = false;
            }
        } else {
            String errorMsg = "OTA failed: " + String(Update.errorString());
            LOG_ERROR("OTAManager: " + errorMsg);
            // 触发OTA升级失败系统事件
#if FASTBEE_ENABLE_PERIPH_EXEC
            PeriphExecManager::getInstance().triggerEvent(EventType::EVENT_OTA_FAILED, errorMsg);
#endif
            request->send(500, "application/json", "{\"error\": \"" + errorMsg + "\"}");
            otaInProgress = false;
        }
    } else {
        // 上传中，返回进度信息
        if (index % (512 * 1024) == 0) { // 每512KB报告一次
            AsyncWebServerResponse *response = request->beginResponse(200, "application/json", 
                "{\"progress\": " + String(progress) + ", \"received\": " + String(currentSize) + "}");
            request->send(response);
        }
    }
}

// 处理OTA状态查询
void OTAManager::handleOTAStatus(AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(256);
    
    doc["in_progress"] = otaInProgress;
    doc["start_time"] = otaStartTime;
    doc["elapsed_ms"] = millis() - otaStartTime;
    doc["current_size"] = currentSize;
    doc["total_size"] = totalSize;
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
        request->send(200, "application/json", "{\"status\": \"No OTA in progress\"}");
        return;
    }
    
    LOG_WARNING("OTAManager: OTA update cancelled by user");
    Update.end(false);
    otaInProgress = false;
    
    request->send(200, "application/json", "{\"status\": \"OTA cancelled\"}");
}

// 通过URL进行OTA更新
bool OTAManager::startOTA(const String& url) {
    if (otaInProgress) {
        LOG_WARNING("OTAManager: OTA already in progress");
        return false;
    }
    
    LOG_INFO("OTAManager: Starting OTA from URL: " + url);
    
    otaInProgress = true;
    otaStartTime = millis();
    otaStatus = OTA_DOWNLOADING;
    otaFirmwareUrl = url;
    otaErrorMessage = "";
    currentSize = 0;
    progress = 0;
    
    WiFiClient client;
    HTTPClient http;
    
    http.begin(client, url);
    http.setTimeout(30000);  // 30秒超时
    
    int httpCode = http.GET();
    
    if (httpCode != HTTP_CODE_OK) {
        otaErrorMessage = "HTTP error: " + String(httpCode);
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        otaInProgress = false;
        http.end();
        return false;
    }
    
    totalSize = http.getSize();
    LOG_INFO("OTAManager: Firmware size: " + String(totalSize) + " bytes");
    
    if (totalSize <= 0) {
        otaErrorMessage = "Invalid firmware size";
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        otaInProgress = false;
        http.end();
        return false;
    }
    
    // 开始更新
    if (!Update.begin(totalSize)) {
        otaErrorMessage = "Update begin failed: " + String(Update.errorString());
        LOG_ERROR("OTAManager: " + otaErrorMessage);
        otaStatus = OTA_FAILED;
        otaInProgress = false;
        http.end();
        return false;
    }
    
    // 设置进度回调
    Update.onProgress([this](size_t prog, size_t total) {
        this->currentSize = prog;
        this->progress = total > 0 ? (prog * 100 / total) : 0;
        if (this->progress % 10 == 0) {
            LOG_INFO("OTAManager: Download progress: " + String(this->progress) + "%");
        }
    });
    
    // 获取流并写入
    WiFiClient* stream = http.getStreamPtr();
    uint8_t buff[1024];
    int bytesRead = 0;
    
    while (http.connected() && (totalSize > 0 || totalSize == -1)) {
        size_t available = stream->available();
        if (available) {
            int c = stream->readBytes(buff, ((available > sizeof(buff)) ? sizeof(buff) : available));
            if (Update.write(buff, c) != c) {
                otaErrorMessage = "Write failed: " + String(Update.errorString());
                LOG_ERROR("OTAManager: " + otaErrorMessage);
                Update.end(false);
                otaStatus = OTA_FAILED;
                otaInProgress = false;
                http.end();
                return false;
            }
            
            currentSize += c;
            progress = totalSize > 0 ? (currentSize * 100 / totalSize) : 0;
            
            if (totalSize > 0) {
                totalSize -= c;
            }
        }
        delay(1);
    }
    
    http.end();
    
    if (Update.end(true)) {
        if (Update.isFinished()) {
            LOG_INFO("OTAManager: OTA update completed successfully");
            otaStatus = OTA_COMPLETED;
            progress = 100;
            
            // 延迟重启
            delay(1000);
            ESP.restart();
            return true;
        }
    }
    
    otaErrorMessage = "Update failed: " + String(Update.errorString());
    LOG_ERROR("OTAManager: " + otaErrorMessage);
    otaStatus = OTA_FAILED;
    otaInProgress = false;
    return false;
}

// 获取OTA状态字符串
String OTAManager::getOTAStatus() {
    if (otaInProgress) {
        return "OTA in progress, " + String(progress) + "% complete (" + 
               String(currentSize / 1024) + "KB/" + String(totalSize / 1024) + "KB)";
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
    return totalSize;
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