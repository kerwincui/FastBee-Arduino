#ifndef SSE_ROUTE_HANDLER_H
#define SSE_ROUTE_HANDLER_H

#include <ESPAsyncWebServer.h>
#include <AsyncEventSource.h>

class WebHandlerContext;
class HealthMonitor;

class SSERouteHandler {
public:
    explicit SSERouteHandler(WebHandlerContext* ctx);
    void setupRoutes(AsyncWebServer* server);

    void broadcastModbusData(const String& data);
    void broadcastMqttStatus(const String& data);
    void broadcastModbusStatus(const String& data);
    size_t clientCount() const;

    /// 周期性维护：发送心跳、清理超时连接、上报 SSE 客户端数
    /// 应在 WebConfigManager::performMaintenance() 中定期调用
    void performMaintenance();

    /// 强制关闭所有 SSE 客户端连接（释放 lwip TCP 缓冲区）
    /// 由 HealthMonitor 在内存进入 SEVERE 时调用
    /// @return 实际被关闭的客户端数
    size_t closeAllClients();

    // 最大 SSE 客户端数，防止多客户端导致内存压力
    static constexpr size_t MAX_SSE_CLIENTS = 2;
    // 单次消息最大字节数，超过则截断
    static constexpr size_t MAX_SSE_MESSAGE_SIZE = 512;
    // 心跳间隔（毫秒）
    static constexpr unsigned long HEARTBEAT_INTERVAL_MS = 60000;
    // 超时阈值：2 个心跳周期无响应则清理
    static constexpr unsigned long CLIENT_TIMEOUT_MS = 120000;
    // 低内存保护阈值（字节）
    static constexpr size_t LOW_MEMORY_THRESHOLD = 15360;

private:
    /// 轻量级客户端活动追踪（与 MAX_SSE_CLIENTS 对齐）
    struct ClientSlot {
        AsyncEventSourceClient* client = nullptr;
        unsigned long connectTime = 0;   // millis() at connect
        unsigned long lastActiveMs = 0;  // 最后一次确认活跃的时间
    };
    ClientSlot _slots[MAX_SSE_CLIENTS];

    WebHandlerContext* ctx;
    AsyncEventSource _events;
    uint32_t _messageId;
    unsigned long _lastHeartbeatMs = 0;

    /// 获取 HealthMonitor 指针（通过 FastBeeFramework 单例）
    HealthMonitor* getHealthMonitor() const;
    /// 向 HealthMonitor 上报当前 SSE 客户端数
    void reportClientCount();
    /// 清理超时 / 僵尸连接
    void cleanupStaleConnections(unsigned long now);
    /// 注册新客户端到跟踪槽位（LRU 满时踢最旧）
    void trackClient(AsyncEventSourceClient* client, unsigned long now);
    /// 从跟踪槽位移除断开的客户端
    void untrackClient(AsyncEventSourceClient* client);
    /// 低内存时是否应跳过 SSE 推送
    bool shouldSkipBroadcast() const;
};

#endif // SSE_ROUTE_HANDLER_H
