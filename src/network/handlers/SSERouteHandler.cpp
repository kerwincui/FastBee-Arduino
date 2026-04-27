#include "./network/handlers/SSERouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include <esp_heap_caps.h>
#if FASTBEE_ENABLE_HEALTH_MONITOR
#include "systems/HealthMonitor.h"
#include "core/FastBeeFramework.h"
#endif

SSERouteHandler::SSERouteHandler(WebHandlerContext* ctx)
    : ctx(ctx), _events("/api/events"), _messageId(0) {
    memset(_slots, 0, sizeof(_slots));
}

void SSERouteHandler::setupRoutes(AsyncWebServer* server) {
    if (!server) {
        return;
    }

    _events.onConnect([this](AsyncEventSourceClient* client) {
        unsigned long now = millis();
        trackClient(client, now);
        LOG_INFOF("SSE client connected, id: %p (total: %u)", static_cast<void*>(client), _events.count());
        reportClientCount();
    });

    _events.onDisconnect([this](AsyncEventSourceClient* client) {
        untrackClient(client);
        LOG_INFO("SSE client disconnected");
        reportClientCount();
    });

    _events.authorizeConnect([this](AsyncWebServerRequest* request) {
        if (!ctx || !request) {
            return false;
        }
        // 并发连接限制：所有槽位被活跃连接占满时拒绝（503 语义）
        uint8_t activeCount = 0;
        for (const auto& s : _slots) {
            if (s.client && s.client->connected()) {
                activeCount++;
            }
        }
        if (activeCount >= MAX_SSE_CLIENTS) {
            LOG_WARNINGF("[SSE] Connection rejected: %u/%u slots occupied",
                         activeCount, (unsigned)MAX_SSE_CLIENTS);
            request->send(503, "text/plain", "Too many SSE connections");
            return false;
        }
        // 认证检查
        if (ctx->authManager) {
            if (ctx->requiresAuth(request)) {
                AuthResult authResult = ctx->authenticateRequest(request);
                return authResult.success;
            }
        }
        return true;
    });

    server->addHandler(&_events);
}

void SSERouteHandler::broadcastModbusData(const String& data) {
    if (_events.count() == 0 || shouldSkipBroadcast()) {
        return;
    }
    // 限制消息大小，避免大数据包阻塞 async_tcp
    if (data.length() > MAX_SSE_MESSAGE_SIZE) {
        LOG_WARNING("SSE: modbus-data message too large, truncated");
        String truncated = data.substring(0, MAX_SSE_MESSAGE_SIZE);
        _events.send(truncated.c_str(), "modbus-data", _messageId++);
        return;
    }
    _events.send(data.c_str(), "modbus-data", _messageId++);
}

void SSERouteHandler::broadcastMqttStatus(const String& data) {
    if (_events.count() == 0 || shouldSkipBroadcast()) {
        return;
    }
    _events.send(data.c_str(), "mqtt-status", _messageId++);
}

void SSERouteHandler::broadcastModbusStatus(const String& data) {
    if (_events.count() == 0 || shouldSkipBroadcast()) {
        return;
    }
    _events.send(data.c_str(), "modbus-status", _messageId++);
}

size_t SSERouteHandler::clientCount() const {
    return _events.count();
}

void SSERouteHandler::performMaintenance() {
    unsigned long now = millis();

    // 1. 清理超时 / 僵尸连接
    cleanupStaleConnections(now);

    // 2. 发送心跳（已有逻辑）
    if (now - _lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        _lastHeartbeatMs = now;
        if (_events.count() > 0) {
            _events.send("ping", "heartbeat", _messageId++);
            // 心跳成功发送 → 刷新所有在线客户端的活跃时间
            for (auto& s : _slots) {
                if (s.client && s.client->connected()) {
                    s.lastActiveMs = now;
                }
            }
        }
        reportClientCount();
    }
}

HealthMonitor* SSERouteHandler::getHealthMonitor() const {
#if FASTBEE_ENABLE_HEALTH_MONITOR
    FastBeeFramework* fw = FastBeeFramework::getInstance();
    return fw ? fw->getHealthMonitor() : nullptr;
#else
    return nullptr;
#endif
}

// --------------- 内部辅助方法 ---------------

void SSERouteHandler::cleanupStaleConnections(unsigned long now) {
    for (auto& s : _slots) {
        if (!s.client) continue;
        // 已断开 → 直接清理槽位
        if (!s.client->connected()) {
            LOG_INFOF("[SSE] Removing disconnected client %p", static_cast<void*>(s.client));
            s.client = nullptr;
            continue;
        }
        // 超时 → 主动关闭
        if (now - s.lastActiveMs >= CLIENT_TIMEOUT_MS) {
            LOG_WARNINGF("[SSE] Client %p timed out (%lu ms), closing",
                         static_cast<void*>(s.client),
                         (unsigned long)(now - s.lastActiveMs));
            s.client->close();
            s.client = nullptr;
        }
    }
}

void SSERouteHandler::trackClient(AsyncEventSourceClient* client, unsigned long now) {
    // 先找空槽
    for (auto& s : _slots) {
        if (!s.client || !s.client->connected()) {
            s.client = client;
            s.connectTime = now;
            s.lastActiveMs = now;
            return;
        }
    }
    // 所有槽位占满 → LRU：踢最早连接的客户端
    size_t oldest = 0;
    for (size_t i = 1; i < MAX_SSE_CLIENTS; ++i) {
        if (_slots[i].connectTime < _slots[oldest].connectTime) {
            oldest = i;
        }
    }
    LOG_WARNINGF("[SSE] Evicting oldest client %p (connected %lu ms ago)",
                 static_cast<void*>(_slots[oldest].client),
                 (unsigned long)(now - _slots[oldest].connectTime));
    _slots[oldest].client->close();
    _slots[oldest].client = client;
    _slots[oldest].connectTime = now;
    _slots[oldest].lastActiveMs = now;
}

void SSERouteHandler::untrackClient(AsyncEventSourceClient* client) {
    for (auto& s : _slots) {
        if (s.client == client) {
            s.client = nullptr;
            return;
        }
    }
}

bool SSERouteHandler::shouldSkipBroadcast() const {
    // 使用最大可分配块而非总空闲堆，更准确地反映能否进行网络操作
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    if (largestBlock < 8192) {
        LOG_WARNINGF("[SSE] Low memory (largestBlock=%lu), skipping broadcast", (unsigned long)largestBlock);
        return true;
    }
    return false;
}

void SSERouteHandler::reportClientCount() {
#if FASTBEE_ENABLE_HEALTH_MONITOR
    HealthMonitor* hm = getHealthMonitor();
    if (hm) {
        hm->setSseClientCount(static_cast<uint8_t>(_events.count()));
    }
#endif
}
