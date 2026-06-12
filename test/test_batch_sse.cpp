/**
 * @file test_batch_sse.cpp
 * @brief BatchRouteHandler 批量请求 & SSERouteHandler 推送事件 单元测试
 * 
 * 测试内容：
 * - 批量请求URL解析与合并逻辑
 * - SSE客户端连接管理
 * - SSE统计信息
 * - SSE内存保护逻辑
 * - SSE心跳和超时机制
 */

#include <unity.h>
#include <Arduino.h>

void test_batch_sse_group();

// ========== 模拟 Batch URL 解析逻辑 ==========

struct BatchRequest {
    String url;
    bool valid;
};

// 镜像 BatchRouteHandler 的 URL 校验逻辑
static bool isValidBatchUrl(const String& url) {
    if (url.isEmpty()) return false;
    if (!url.startsWith("/api/")) return false;
    if (url.indexOf("..") >= 0) return false;  // 防路径遍历
    if (url.length() > 128) return false;  // 长度限制
    return true;
}

// 模拟批量请求解析
static std::vector<BatchRequest> parseBatchUrls(const String& jsonArray) {
    std::vector<BatchRequest> requests;
    // 简单解析逗号分隔的URL列表
    int start = 0;
    while (start < (int)jsonArray.length()) {
        int end = jsonArray.indexOf(',', start);
        if (end < 0) end = jsonArray.length();
        String url = jsonArray.substring(start, end);
        url.trim();
        requests.push_back({url, isValidBatchUrl(url)});
        start = end + 1;
    }
    return requests;
}

// ========== 模拟 SSE 客户端管理逻辑 ==========

struct MockSSEStats {
    uint32_t acceptedConnections = 0;
    uint32_t rejectedLowMemory = 0;
    uint32_t rejectedCapacity = 0;
    uint32_t timedOutClients = 0;
    uint32_t disconnectedCleanups = 0;
    size_t currentClients = 0;
};

static constexpr size_t MAX_SSE_CLIENTS = 2;
static constexpr size_t LOW_MEMORY_THRESHOLD = 8192;
static constexpr unsigned long HEARTBEAT_INTERVAL_MS = 60000;
static constexpr unsigned long CLIENT_TIMEOUT_MS = 120000;

class MockSSEManager {
public:
    MockSSEManager() : _freeHeap(32768) {}
    
    bool acceptClient() {
        if (_freeHeap < LOW_MEMORY_THRESHOLD) {
            _stats.rejectedLowMemory++;
            return false;
        }
        if (_stats.currentClients >= MAX_SSE_CLIENTS) {
            _stats.rejectedCapacity++;
            return false;
        }
        _stats.currentClients++;
        _stats.acceptedConnections++;
        return true;
    }
    
    void disconnectClient() {
        if (_stats.currentClients > 0) {
            _stats.currentClients--;
            _stats.disconnectedCleanups++;
        }
    }
    
    bool shouldSkipBroadcast() {
        return _freeHeap < LOW_MEMORY_THRESHOLD;
    }
    
    void performMaintenance(unsigned long now) {
        // 检查超时
        for (auto& slot : _clientTimes) {
            if (slot > 0 && (now - slot) > CLIENT_TIMEOUT_MS) {
                _stats.timedOutClients++;
                _stats.currentClients--;
                slot = 0;
            }
        }
    }
    
    void setFreeHeap(uint32_t heap) { _freeHeap = heap; }
    MockSSEStats getStats() const { return _stats; }
    size_t clientCount() const { return _stats.currentClients; }
    
private:
    MockSSEStats _stats;
    uint32_t _freeHeap;
    unsigned long _clientTimes[MAX_SSE_CLIENTS] = {0};
};

// ========== Batch 测试用例 ==========

static void test_batch_valid_urls() {
    TEST_ASSERT_TRUE(isValidBatchUrl("/api/peripherals"));
    TEST_ASSERT_TRUE(isValidBatchUrl("/api/system/status"));
    TEST_ASSERT_TRUE(isValidBatchUrl("/api/network/config"));
}

static void test_batch_invalid_urls() {
    TEST_ASSERT_FALSE(isValidBatchUrl(""));
    TEST_ASSERT_FALSE(isValidBatchUrl("/other/path"));
    TEST_ASSERT_FALSE(isValidBatchUrl("/api/../etc/passwd"));
    
    // 超长URL
    String longUrl = "/api/";
    for (int i = 0; i < 130; i++) longUrl += "x";
    TEST_ASSERT_FALSE(isValidBatchUrl(longUrl));
}

static void test_batch_parse_multiple() {
    auto requests = parseBatchUrls("/api/status,/api/peripherals,/api/network");
    TEST_ASSERT_EQUAL(3, requests.size());
    TEST_ASSERT_TRUE(requests[0].valid);
    TEST_ASSERT_TRUE(requests[1].valid);
    TEST_ASSERT_TRUE(requests[2].valid);
}

static void test_batch_parse_mixed_validity() {
    auto requests = parseBatchUrls("/api/ok,/invalid,/api/good");
    TEST_ASSERT_EQUAL(3, requests.size());
    TEST_ASSERT_TRUE(requests[0].valid);
    TEST_ASSERT_FALSE(requests[1].valid);
    TEST_ASSERT_TRUE(requests[2].valid);
}

static void test_batch_single_url() {
    auto requests = parseBatchUrls("/api/single");
    TEST_ASSERT_EQUAL(1, requests.size());
    TEST_ASSERT_TRUE(requests[0].valid);
    TEST_ASSERT_EQUAL_STRING("/api/single", requests[0].url.c_str());
}

// ========== SSE 测试用例 ==========

static void test_sse_accept_client() {
    MockSSEManager sse;
    
    TEST_ASSERT_TRUE(sse.acceptClient());
    TEST_ASSERT_EQUAL(1, sse.clientCount());
    
    TEST_ASSERT_TRUE(sse.acceptClient());
    TEST_ASSERT_EQUAL(2, sse.clientCount());
}

static void test_sse_reject_capacity() {
    MockSSEManager sse;
    
    sse.acceptClient();
    sse.acceptClient();
    
    // 第3个应该被拒绝
    TEST_ASSERT_FALSE(sse.acceptClient());
    TEST_ASSERT_EQUAL(2, sse.clientCount());
    
    auto stats = sse.getStats();
    TEST_ASSERT_EQUAL(1, stats.rejectedCapacity);
}

static void test_sse_reject_low_memory() {
    MockSSEManager sse;
    sse.setFreeHeap(4096);  // 低于 8192 阈值
    
    TEST_ASSERT_FALSE(sse.acceptClient());
    TEST_ASSERT_EQUAL(0, sse.clientCount());
    
    auto stats = sse.getStats();
    TEST_ASSERT_EQUAL(1, stats.rejectedLowMemory);
}

static void test_sse_disconnect_client() {
    MockSSEManager sse;
    
    sse.acceptClient();
    sse.acceptClient();
    TEST_ASSERT_EQUAL(2, sse.clientCount());
    
    sse.disconnectClient();
    TEST_ASSERT_EQUAL(1, sse.clientCount());
    
    auto stats = sse.getStats();
    TEST_ASSERT_EQUAL(1, stats.disconnectedCleanups);
}

static void test_sse_skip_broadcast_low_memory() {
    MockSSEManager sse;
    
    sse.setFreeHeap(32768);
    TEST_ASSERT_FALSE(sse.shouldSkipBroadcast());
    
    sse.setFreeHeap(4096);
    TEST_ASSERT_TRUE(sse.shouldSkipBroadcast());
}

static void test_sse_stats_tracking() {
    MockSSEManager sse;
    
    sse.acceptClient();
    sse.acceptClient();
    sse.acceptClient();  // rejected
    sse.disconnectClient();
    
    auto stats = sse.getStats();
    TEST_ASSERT_EQUAL(2, stats.acceptedConnections);
    TEST_ASSERT_EQUAL(1, stats.rejectedCapacity);
    TEST_ASSERT_EQUAL(1, stats.disconnectedCleanups);
    TEST_ASSERT_EQUAL(1, stats.currentClients);
}

static void test_sse_constants() {
    TEST_ASSERT_EQUAL(2, MAX_SSE_CLIENTS);
    TEST_ASSERT_EQUAL(8192, LOW_MEMORY_THRESHOLD);
    TEST_ASSERT_EQUAL(60000, HEARTBEAT_INTERVAL_MS);
    TEST_ASSERT_EQUAL(120000, CLIENT_TIMEOUT_MS);
}

// ========== 测试组入口 ==========

void test_batch_sse_group() {
    // Batch tests
    RUN_TEST(test_batch_valid_urls);
    RUN_TEST(test_batch_invalid_urls);
    RUN_TEST(test_batch_parse_multiple);
    RUN_TEST(test_batch_parse_mixed_validity);
    RUN_TEST(test_batch_single_url);
    
    // SSE tests
    RUN_TEST(test_sse_accept_client);
    RUN_TEST(test_sse_reject_capacity);
    RUN_TEST(test_sse_reject_low_memory);
    RUN_TEST(test_sse_disconnect_client);
    RUN_TEST(test_sse_skip_broadcast_low_memory);
    RUN_TEST(test_sse_stats_tracking);
    RUN_TEST(test_sse_constants);
}
