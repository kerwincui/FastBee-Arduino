#include "./network/handlers/SSERouteHandler.h"
#include "./network/WebHandlerContext.h"
#include "network/SseJsonChunks.h"
#include "systems/LoggerSystem.h"
#include "core/FeatureFlags.h"
#include "core/MemoryBudget.h"
#include <cstdio>
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
        _stats.acceptedConnections++;
        _stats.lastConnectAtMs = now;
        LOG_INFOF("SSE client connected, id: %p (total: %u)", static_cast<void*>(client), _events.count());
        reportClientCount();
    });

    _events.onDisconnect([this](AsyncEventSourceClient* client) {
        untrackClient(client);
        LOG_INFO("SSE client disconnected");
        reportClientCount();
    });

    // authorizeConnect maps every false result to 401, overwriting resource
    // rejection responses. Middleware preserves 503 without bypassing auth.
    _events.addMiddleware([this](AsyncWebServerRequest* request, ArMiddlewareNext next) {
        if (!ctx || !request) {
            if (request) request->send(503, "text/plain", "SSE unavailable");
            return;
        }
        // 内存门控：内部 8BIT 最大连续块 < 8KB 或 MEMGUARD >= SEVERE 时拒绝新 SSE
        // 避免浏览器多 tab 自动重连持续吃堆，让系统有恢复窗口
        const uint32_t largestBlock = FastBee::MemoryBudget::dram8Largest();
        if (largestBlock < LOW_MEMORY_THRESHOLD) {
            _stats.rejectedLowMemory++;
            _stats.lastRejectAtMs = millis();
            setReason(_stats.lastRejectReason, sizeof(_stats.lastRejectReason), "low_memory");
            LOG_WARNINGF("[SSE] Connection rejected: low memory (largestBlock=%lu < %u)",
                         (unsigned long)largestBlock, (unsigned)LOW_MEMORY_THRESHOLD);
            request->send(503, "text/plain", "Server low memory, retry later");
            return;
        }
#if FASTBEE_ENABLE_HEALTH_MONITOR
        HealthMonitor* hm = getHealthMonitor();
        if (hm && (hm->isMemorySevere() || hm->isMemoryCritical())) {
            _stats.rejectedGuard++;
            _stats.lastRejectAtMs = millis();
            setReason(_stats.lastRejectReason, sizeof(_stats.lastRejectReason), "guard_active");
            LOG_WARNING("[SSE] Connection rejected: MEMGUARD SEVERE/CRITICAL active");
            request->send(503, "text/plain", "Server memory guard active, retry later");
            return;
        }
#endif
        // 并发连接限制：所有槽位被活跃连接占满时拒绝（503 语义）
        uint8_t activeCount = 0;
        for (const auto& s : _slots) {
            if (s.client && s.client->connected()) {
                activeCount++;
            }
        }
        if (activeCount >= MAX_SSE_CLIENTS) {
            _stats.rejectedCapacity++;
            _stats.lastRejectAtMs = millis();
            setReason(_stats.lastRejectReason, sizeof(_stats.lastRejectReason), "capacity");
            LOG_WARNINGF("[SSE] Connection rejected: %u/%u slots occupied",
                         activeCount, (unsigned)MAX_SSE_CLIENTS);
            request->send(503, "text/plain", "Too many SSE connections");
            return;
        }
        // 认证检查
        if (ctx->authManager) {
            if (ctx->requiresAuth(request)) {
                AuthResult authResult = ctx->authenticateRequest(request);
                if (!authResult.success) {
                    request->send(401, "text/plain", "Unauthorized");
                    return;
                }
            }
        }
        ctx->noteWebRequestActivity();
        next();
    });

    server->addHandler(&_events);
}

void SSERouteHandler::broadcastModbusData(const String& data) {
    if (_events.count() == 0 || shouldSkipBroadcast()) {
        return;
    }
    // Retain the memory budget without ever truncating a JSON record.
    if (data.length() > MAX_SSE_MESSAGE_SIZE) {
        const auto result = FastBee::sendSseObjectArray<MAX_SSE_MESSAGE_SIZE>(
            data.c_str(), data.length(), [this](const char* chunk) {
                return _events.send(chunk, "modbus-data", _messageId++) == AsyncEventSource::ENQUEUED;
            });
        if (result != FastBee::SseChunkResult::Ok) {
            _stats.lastSkipBroadcastAtMs = millis();
            setReason(_stats.lastSkipReason, sizeof(_stats.lastSkipReason), "modbus_chunk_failed");
            LOG_WARNING("SSE: incomplete Modbus publication; requesting snapshot recovery");
            _events.send("{\"reason\":\"modbus_chunk_failed\"}", "resync", _messageId++);
        }
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

void SSERouteHandler::broadcastSensorData(const String& data) {
    if (_events.count() == 0 || shouldSkipBroadcast()) {
        return;
    }
    _events.send(data.c_str(), "sensor-data", _messageId++);
}

size_t SSERouteHandler::clientCount() const {
    return _events.count();
}

size_t SSERouteHandler::closeAllClients() {
    size_t closed = 0;
    unsigned long now = millis();
    for (auto& s : _slots) {
        if (s.client) {
            if (s.client->connected()) {
                s.client->close();
                closed++;
            }
            s = ClientSlot{};
        }
    }
    if (closed > 0) {
        _stats.forcedClosedClients += static_cast<uint32_t>(closed);
        _stats.lastForcedCloseAtMs = now;
        LOG_WARNINGF("[SSE] Force-closed %u clients (memory pressure)", (unsigned)closed);
    }
    return closed;
}

void SSERouteHandler::performMaintenance() {
    unsigned long now = millis();

    // 1. 清理超时 / 僵尸连接
    cleanupStaleConnections(now);

    // 2. 发送心跳
    if (now - _lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
        _lastHeartbeatMs = now;
        if (_events.count() > 0) {
            _events.send("ping", "heartbeat", _messageId++);
            // Only positive TCP ACKs update activity. Queueing a heartbeat
            // must not keep a blocked peer alive.
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
            _stats.disconnectedCleanups++;
            _stats.lastCleanupAtMs = now;
            s = ClientSlot{};
            continue;
        }
        // 超时 → 主动关闭
        if (now - s.lastActiveMs >= CLIENT_TIMEOUT_MS) {
            LOG_WARNINGF("[SSE] Client %p timed out (%lu ms), closing",
                         static_cast<void*>(s.client),
                         (unsigned long)(now - s.lastActiveMs));
            s.client->close();
            _stats.timedOutClients++;
            _stats.lastCleanupAtMs = now;
            s = ClientSlot{};
        }
    }
}

void SSERouteHandler::trackClient(AsyncEventSourceClient* client, unsigned long now) {
    if (!client || !client->client()) return;
    // Keep ESPAsyncWebServer's ACK bookkeeping when adding liveness tracking.
    client->client()->onAck([this, client](void*, AsyncClient*, size_t length, uint32_t time) {
        noteClientAck(client, length);
        client->_onAck(length, time);
    }, nullptr);
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
    _stats.evictedOldestClients++;
    _stats.lastForcedCloseAtMs = now;
    _slots[oldest].client = client;
    _slots[oldest].connectTime = now;
    _slots[oldest].lastActiveMs = now;
}

void SSERouteHandler::noteClientAck(AsyncEventSourceClient* client, size_t length) {
    if (length == 0) return;
    for (auto& slot : _slots) {
        if (slot.client == client) {
            slot.lastActiveMs = millis();
            return;
        }
    }
}

void SSERouteHandler::untrackClient(AsyncEventSourceClient* client) {
    for (auto& s : _slots) {
        if (s.client == client) {
            s = ClientSlot{};
            return;
        }
    }
}

bool SSERouteHandler::shouldSkipBroadcast() {
    const unsigned long now = millis();

#if FASTBEE_ENABLE_HEALTH_MONITOR
    HealthMonitor* hm = getHealthMonitor();
    if (hm && (hm->isMemorySevere() || hm->isMemoryCritical())) {
        _stats.skippedBroadcastGuard++;
        _stats.lastSkipBroadcastAtMs = now;
        setReason(_stats.lastSkipReason, sizeof(_stats.lastSkipReason), "guard_active");
        if (_lastSkipLogMs == 0 || now - _lastSkipLogMs >= 10000UL) {
            LOG_WARNING("[SSE] MEMGUARD SEVERE/CRITICAL active, skipping broadcast");
            _lastSkipLogMs = now;
        }
        return true;
    }
#endif
    // 网络操作需要内部 8BIT 连续块；PSRAM 或仅支持 32BIT 的内部堆不能补足。
    const uint32_t largestBlock = FastBee::MemoryBudget::dram8Largest();
    if (largestBlock < LOW_MEMORY_THRESHOLD) {
        _stats.skippedBroadcastLowMemory++;
        _stats.lastSkipBroadcastAtMs = now;
        setReason(_stats.lastSkipReason, sizeof(_stats.lastSkipReason), "low_memory");
        if (_lastSkipLogMs == 0 || now - _lastSkipLogMs >= 10000UL) {
            LOG_WARNINGF("[SSE] Low memory (largestBlock=%lu), skipping broadcast",
                         (unsigned long)largestBlock);
            _lastSkipLogMs = now;
        }
        return true;
    }
    return false;
}

void SSERouteHandler::setReason(char* dest, size_t destSize, const char* reason) const {
    if (!dest || destSize == 0) {
        return;
    }
    snprintf(dest, destSize, "%s", reason ? reason : "");
}

void SSERouteHandler::reportClientCount() {
#if FASTBEE_ENABLE_HEALTH_MONITOR
    HealthMonitor* hm = getHealthMonitor();
    if (hm) {
        hm->setSseClientCount(static_cast<uint8_t>(_events.count()));
    }
#endif
}
