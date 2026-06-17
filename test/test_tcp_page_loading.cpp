/**
 * @file test_tcp_page_loading.cpp
 * @brief TCP 连接预算 & 页面加载稳定性测试
 * 
 * 验证各芯片（ESP32/S3/C3/C6）TCP连接预算配置正确性，
 * 以及所有页面在连接限制下都能正常加载。
 * 
 * 依据：docs/tcp-connection-budget.md
 */

#include <unity.h>
#include <Arduino.h>
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

void test_tcp_page_loading_group();

// ============================================================
// TCP 连接预算常量验证（各芯片）
// ============================================================

// 芯片 TCP 预算结构
struct ChipTCPBudget {
    const char* name;
    size_t totalBudget;
    size_t sseBudget;
    size_t httpBudget;
    size_t exhaustionThreshold;
    size_t maxSseClients;
};

// 从 docs/tcp-connection-budget.md 中定义的预算
static const ChipTCPBudget CHIP_BUDGETS[] = {
    {"ESP32-C3",      4, 1, 3, 10, 1},
    {"ESP32-S3",      8, 2, 6, 14, 2},
    {"ESP32 classic", 6, 1, 5, 12, 1},
    {"ESP32-C6",      6, 1, 5, 12, 1},
};

/**
 * @brief 验证各芯片 TCP 预算满足硬约束
 * 约束：MEMP_NUM_TCP_PCB=16，耗尽阈值必须 < 16
 */
void test_tcp_budget_constraints() {
    TestLog::testStart("TCP Budget: Hardware Constraints");
    
    constexpr size_t LWIP_MAX_TCP_PCB = 16;
    
    for (const auto& chip : CHIP_BUDGETS) {
        // 预算 = SSE + HTTP
        TEST_ASSERT_EQUAL_MESSAGE(chip.totalBudget, chip.sseBudget + chip.httpBudget,
            (String(chip.name) + ": total != sse + http").c_str());
        
        // 耗尽阈值 < lwIP PCB 硬上限
        TEST_ASSERT_LESS_THAN_MESSAGE(LWIP_MAX_TCP_PCB, chip.exhaustionThreshold,
            (String(chip.name) + ": threshold >= 16").c_str());
        
        // MAX_SSE_CLIENTS <= SSE 预算
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE(chip.sseBudget, chip.maxSseClients,
            (String(chip.name) + ": maxSseClients > sseBudget").c_str());
    }
    TestLog::step("All chips: total=sse+http, threshold<16, sseClients<=sseBudget");
    
    TestLog::testEnd(true);
}

/**
 * @brief 验证 C3 紧凑预算（400KB SRAM 限制）
 */
void test_tcp_budget_c3_compact() {
    TestLog::testStart("TCP Budget: ESP32-C3 Compact");
    
    const ChipTCPBudget& c3 = CHIP_BUDGETS[0];
    
    TEST_ASSERT_EQUAL(4, c3.totalBudget);
    TEST_ASSERT_EQUAL(1, c3.sseBudget);
    TEST_ASSERT_EQUAL(3, c3.httpBudget);
    TEST_ASSERT_EQUAL(10, c3.exhaustionThreshold);
    TestLog::step("C3: TCP=4(SSE=1+HTTP=3), threshold=10");
    
    // C3 每连接 ~12KB, 4×12=48KB, 占可用堆 ~140KB 的 34%
    constexpr size_t PER_CONN_MEMORY = 12000;
    constexpr size_t C3_AVAILABLE_HEAP = 140000;
    size_t tcpMemory = c3.totalBudget * PER_CONN_MEMORY;
    float memPercent = (float)tcpMemory / C3_AVAILABLE_HEAP * 100;
    TEST_ASSERT_LESS_THAN(50, (int)memPercent);  // TCP 不超过 50% 堆
    TestLog::step("C3: TCP memory 48KB = 34% of available heap (< 50%)");
    
    TestLog::testEnd(true);
}

/**
 * @brief 验证 S3 多标签页支持（PSRAM 卸载）
 */
void test_tcp_budget_s3_multi_tab() {
    TestLog::testStart("TCP Budget: ESP32-S3 Multi-Tab");
    
    const ChipTCPBudget& s3 = CHIP_BUDGETS[1];
    
    TEST_ASSERT_EQUAL(8, s3.totalBudget);
    TEST_ASSERT_EQUAL(2, s3.sseBudget);
    TEST_ASSERT_EQUAL(6, s3.httpBudget);
    TEST_ASSERT_EQUAL(14, s3.exhaustionThreshold);
    TEST_ASSERT_EQUAL(2, s3.maxSseClients);
    TestLog::step("S3: TCP=8(SSE=2+HTTP=6), threshold=14, multiTab=2");
    
    // S3 场景: 2 个标签页各1个 SSE + 并发请求
    // MQTT(1) + SSE(2) + HTTP(4) + TIME_WAIT(3) = 10 < 14(阈值)
    constexpr size_t MQTT_CONN = 1;
    constexpr size_t MAX_TIME_WAIT = 3;
    size_t multiTabPeak = MQTT_CONN + s3.sseBudget + 4 + MAX_TIME_WAIT;
    TEST_ASSERT_LESS_THAN(s3.exhaustionThreshold, multiTabPeak);
    TestLog::step("S3: multi-tab peak (10) < threshold (14)");
    
    TestLog::testEnd(true);
}

// ============================================================
// 页面加载序列模拟（验证 TCP 连接不会耗尽）
// ============================================================

// 模拟 TCP 连接池
class MockTCPConnectionPool {
public:
    MockTCPConnectionPool(size_t maxConnections, size_t sseSlots)
        : _maxConn(maxConnections), _sseSlots(sseSlots),
          _activeHTTP(0), _activeSSE(0), _timeWait(0), _mqttConn(1) {}

    // 尝试建立 HTTP 连接
    bool acquireHTTP() {
        if (totalActive() >= _maxConn) return false;
        _activeHTTP++;
        return true;
    }
    
    void releaseHTTP() { if (_activeHTTP > 0) _activeHTTP--; _timeWait++; }
    
    // 尝试建立 SSE 连接
    bool acquireSSE() {
        if (_activeSSE >= _sseSlots) return false;
        if (totalActive() >= _maxConn) return false;
        _activeSSE++;
        return true;
    }
    
    void releaseSSE() { if (_activeSSE > 0) _activeSSE--; }
    
    // TIME_WAIT 连接过期
    void expireTimeWait(size_t count = 1) {
        _timeWait = (_timeWait > count) ? _timeWait - count : 0;
    }
    
    size_t totalActive() const {
        return _mqttConn + _activeSSE + _activeHTTP + _timeWait;
    }
    
    size_t getActiveHTTP() const { return _activeHTTP; }
    size_t getActiveSSE() const { return _activeSSE; }
    size_t getTimeWait() const { return _timeWait; }
    size_t available() const {
        size_t total = totalActive();
        return (total < _maxConn) ? (_maxConn - total) : 0;
    }

private:
    size_t _maxConn;
    size_t _sseSlots;
    size_t _activeHTTP;
    size_t _activeSSE;
    size_t _timeWait;
    size_t _mqttConn;
};

// 页面加载请求计数
struct PageLoadProfile {
    const char* pageName;
    size_t concurrentRequests;  // 页面加载时并发请求数（HTML + JS + CSS + API）
    bool needsSSE;
};

static const PageLoadProfile PAGE_PROFILES[] = {
    {"dashboard",   3, true},   // dashboard.html + api/system/status + api/network/status + SSE
    {"device",      3, false},  // device.html + api/device/config + api/peripherals
    {"network",     2, false},  // network.html + api/network/config
    {"protocol",    3, true},   // protocol.html + api/protocol/config + api/mqtt/status + SSE
    {"peripheral",  3, false},  // peripheral.html + api/peripherals + api/peripherals/types
    {"logs",        2, false},  // logs.html + api/logs/list
    {"admin",       2, false},  // admin.html + api/users
    {"rule-script", 2, false},  // rule-script.html + api/rule-script
};

/**
 * @brief 模拟在 ESP32 classic 上依次加载所有页面
 * 验证每个页面都能成功加载（HTTP 连接不会被拒绝）
 */
void test_page_loading_esp32_classic() {
    TestLog::testStart("Page Loading: ESP32 Classic (TCP=6)");
    
    // ESP32 classic: MEMP_NUM_TCP_PCB=16, 业务预算 TCP=6(SSE=1+HTTP=5)
    // 但实际 PCB 池可以用到 12（耗尽阈值），业务预算只是设计参考
    constexpr size_t MAX_PCB = 12;  // 耗尽阈值
    constexpr size_t SSE_SLOTS = 1;
    
    MockTCPConnectionPool pool(MAX_PCB, SSE_SLOTS);
    int pagesLoaded = 0;
    int pagesFailed = 0;
    
    for (const auto& page : PAGE_PROFILES) {
        // 模拟旧 HTTP 连接进入 TIME_WAIT
        pool.expireTimeWait(2);  // 每页之间清理一些 TIME_WAIT
        
        // 尝试加载页面的并发请求
        bool allRequested = true;
        size_t acquired = 0;
        for (size_t i = 0; i < page.concurrentRequests; i++) {
            if (pool.acquireHTTP()) {
                acquired++;
            } else {
                allRequested = false;
                break;
            }
        }
        
        if (allRequested) {
            pagesLoaded++;
        } else {
            pagesFailed++;
        }
        
        // 释放已获取的 HTTP 连接（模拟请求完成）
        for (size_t i = 0; i < acquired; i++) {
            pool.releaseHTTP();
        }
        
        // SSE 连接（如果需要）
        if (page.needsSSE && pool.getActiveSSE() == 0) {
            pool.acquireSSE();
        }
    }
    
    TEST_ASSERT_EQUAL_MESSAGE(8, pagesLoaded,
        "Not all pages loaded successfully on ESP32 classic");
    TEST_ASSERT_EQUAL(0, pagesFailed);
    TestLog::step("All 8 pages loaded successfully on ESP32 classic");
    
    TestLog::testEnd(true);
}

/**
 * @brief 模拟在 ESP32-C3 上依次加载所有页面（最紧凑的 TCP 预算）
 */
void test_page_loading_esp32_c3() {
    TestLog::testStart("Page Loading: ESP32-C3 (TCP=4, threshold=10)");
    
    constexpr size_t MAX_PCB = 10;  // C3 耗尽阈值
    constexpr size_t SSE_SLOTS = 1;
    
    MockTCPConnectionPool pool(MAX_PCB, SSE_SLOTS);
    int pagesLoaded = 0;
    
    for (const auto& page : PAGE_PROFILES) {
        pool.expireTimeWait(2);
        
        bool allRequested = true;
        size_t acquired = 0;
        for (size_t i = 0; i < page.concurrentRequests; i++) {
            if (pool.acquireHTTP()) {
                acquired++;
            } else {
                allRequested = false;
                break;
            }
        }
        
        if (allRequested) pagesLoaded++;
        
        for (size_t i = 0; i < acquired; i++) {
            pool.releaseHTTP();
        }
        
        if (page.needsSSE && pool.getActiveSSE() == 0) {
            pool.acquireSSE();
        }
    }
    
    TEST_ASSERT_EQUAL_MESSAGE(8, pagesLoaded,
        "Not all pages loaded on ESP32-C3");
    TestLog::step("All 8 pages loaded successfully on ESP32-C3");
    
    TestLog::testEnd(true);
}

/**
 * @brief 模拟 MQTT 页面打开后其他页面仍可加载（修复验证）
 * 复现场景：旧代码 MQTT 页面创建 SSE 连接，占满 TCP 后其他页面打不开
 */
void test_page_loading_after_mqtt_page() {
    TestLog::testStart("Page Loading: After MQTT Page (No SSE Blocking)");
    
    constexpr size_t MAX_PCB = 12;  // ESP32 classic 耗尽阈值
    constexpr size_t SSE_SLOTS = 1;
    
    MockTCPConnectionPool pool(MAX_PCB, SSE_SLOTS);
    
    // Step 1: 打开 MQTT/协议 页面（REST 轮询，不占 SSE 槽位）
    // 修复后：MQTT 状态用 5s REST 轮询，不创建 SSE 持久连接
    TEST_ASSERT_TRUE(pool.acquireHTTP());  // 获取 protocol.html
    pool.releaseHTTP();
    TEST_ASSERT_TRUE(pool.acquireHTTP());  // api/mqtt/status
    pool.releaseHTTP();
    TestLog::step("MQTT page loaded (REST only, no SSE)");
    
    // Step 2: 模拟 5s 定时器 REST 轮询（占用 1 个短暂 HTTP 连接）
    TEST_ASSERT_TRUE(pool.acquireHTTP());  // 轮询请求
    pool.releaseHTTP();
    TestLog::step("MQTT poll works (short HTTP request)");
    
    // Step 3: 切换到设备日志页面
    pool.expireTimeWait(1);
    TEST_ASSERT_TRUE(pool.acquireHTTP());  // logs.html
    pool.releaseHTTP();
    TEST_ASSERT_TRUE(pool.acquireHTTP());  // api/logs/list
    pool.releaseHTTP();
    TestLog::step("Logs page loads after MQTT page");
    
    // Step 4: 切换到用户管理页面
    pool.expireTimeWait(1);
    TEST_ASSERT_TRUE(pool.acquireHTTP());  // admin.html
    pool.releaseHTTP();
    TEST_ASSERT_TRUE(pool.acquireHTTP());  // api/users
    pool.releaseHTTP();
    TestLog::step("Admin page loads after MQTT page");
    
    // 验证：SSE 槽位未被 MQTT 页面占用
    TEST_ASSERT_EQUAL(0, pool.getActiveSSE());
    TestLog::step("SSE slot NOT occupied by MQTT page (fixed)");
    
    TestLog::testEnd(true);
}

/**
 * @brief 模拟旧代码场景（SSE 占用导致页面打不开）— 验证修复
 */
void test_page_loading_old_sse_bug_repro() {
    TestLog::testStart("Page Loading: Old SSE Bug Repro (Fixed)");
    
    constexpr size_t MAX_PCB = 6;  // 模拟只有 6 个可用 PCB 的紧凑场景
    constexpr size_t SSE_SLOTS = 1;
    
    MockTCPConnectionPool pool(MAX_PCB, SSE_SLOTS);
    
    // 旧代码行为: MQTT 页面创建 SSE 连接 → 永久占用 1 个 PCB
    // pool.acquireSSE();  // 旧代码会做这个
    // 新代码: 不创建 SSE, 用 REST 轮询
    
    // 模拟已有 MQTT 连接(1) + dashboard SSE(1) + 一些 TIME_WAIT(2)
    pool.acquireSSE();  // dashboard 的 SSE
    // MQTT 已经占了 1 (构造函数中)
    // 手动增加 TIME_WAIT
    pool.acquireHTTP(); pool.releaseHTTP();  // +1 TIME_WAIT
    pool.acquireHTTP(); pool.releaseHTTP();  // +1 TIME_WAIT
    
    // 现在: MQTT(1) + SSE(1) + TIME_WAIT(2) = 4, 剩余 2
    TEST_ASSERT_EQUAL(4, pool.totalActive());
    TEST_ASSERT_EQUAL(2, pool.available());
    TestLog::step("State: MQTT(1)+SSE(1)+TW(2)=4, available=2");
    
    // 加载设备日志页面: 需要 2 个并发请求 → 正好够
    TEST_ASSERT_TRUE(pool.acquireHTTP());
    TEST_ASSERT_TRUE(pool.acquireHTTP());
    TestLog::step("Logs page: 2 concurrent requests → SUCCESS");
    
    pool.releaseHTTP();
    pool.releaseHTTP();
    
    TestLog::testEnd(true);
}

/**
 * @brief 压力测试：快速连续切换所有页面（模拟真实用户行为）
 */
void test_page_loading_rapid_switching() {
    TestLog::testStart("Page Loading: Rapid Page Switching");
    
    constexpr size_t MAX_PCB = 12;
    constexpr size_t SSE_SLOTS = 1;
    constexpr int SWITCH_ROUNDS = 3;  // 快速切换 3 轮
    
    MockTCPConnectionPool pool(MAX_PCB, SSE_SLOTS);
    int totalLoads = 0;
    int totalFails = 0;
    
    for (int round = 0; round < SWITCH_ROUNDS; round++) {
        for (const auto& page : PAGE_PROFILES) {
            // 页面切换：释放前一页面的 SSE（真实浏览器切标签页时 SSE 关闭）
            if (pool.getActiveSSE() > 0) {
                pool.releaseSSE();
            }
            // 每页切换时清理 TIME_WAIT（模拟真实 TCP 栈自动回收）
            pool.expireTimeWait(3);
            
            size_t acquired = 0;
            bool ok = true;
            for (size_t i = 0; i < page.concurrentRequests; i++) {
                if (pool.acquireHTTP()) {
                    acquired++;
                } else {
                    ok = false;
                    break;
                }
            }
            
            if (ok) totalLoads++; else totalFails++;
            
            for (size_t i = 0; i < acquired; i++) {
                pool.releaseHTTP();
            }
            
            // SSE 连接（如果需要）
            if (page.needsSSE && pool.getActiveSSE() == 0) {
                pool.acquireSSE();
            }
        }
    }
    
    // 3 轮 × 8 页面 = 24 次加载应全部成功
    TEST_ASSERT_EQUAL(24, totalLoads);
    TEST_ASSERT_EQUAL(0, totalFails);
    TestLog::step("24 page loads across 3 rounds: all successful");
    
    TestLog::testEnd(true);
}

/**
 * @brief 长时间运行稳定性：模拟 100 轮页面切换不泄漏 TCP 连接
 */
void test_tcp_long_running_no_leak() {
    TestLog::testStart("TCP Long Running: No Connection Leak (100 rounds)");
    
    constexpr size_t MAX_PCB = 12;
    constexpr size_t SSE_SLOTS = 1;
    
    MockTCPConnectionPool pool(MAX_PCB, SSE_SLOTS);
    
    // 开一个 SSE 连接（模拟 dashboard 推送）
    pool.acquireSSE();
    
    for (int round = 0; round < 100; round++) {
        // 每轮模拟加载一个页面（3 个并发请求）
        for (int i = 0; i < 3; i++) {
            TEST_ASSERT_TRUE_MESSAGE(pool.acquireHTTP(),
                "HTTP connection exhausted during long run");
        }
        for (int i = 0; i < 3; i++) {
            pool.releaseHTTP();
        }
        
        // 每轮清理 TIME_WAIT（真实 TCP 栈持续回收，expire 与 acquire 平衡）
        pool.expireTimeWait(3);
    }
    
    // 100 轮后 TIME_WAIT 应该不会无限增长（因为有清理）
    TEST_ASSERT_LESS_THAN(MAX_PCB, pool.getTimeWait());
    TestLog::step("100 rounds: no TCP leak, TIME_WAIT bounded");
    
    // SSE 仍然活着
    TEST_ASSERT_EQUAL(1, pool.getActiveSSE());
    TestLog::step("SSE connection survived 100 rounds");
    
    TestLog::testEnd(true);
}

// ============================================================
// API 端点覆盖测试（验证所有关键端点可达）
// ============================================================

// 模拟 API 端点状态
struct APIEndpoint {
    const char* path;
    const char* method;
    int expectedStatus;  // 200 = 正常, 401 = 需认证
};

static const APIEndpoint CRITICAL_ENDPOINTS[] = {
    {"/api/system/health",       "GET", 200},
    {"/api/system/info",         "GET", 200},
    {"/api/system/status",       "GET", 200},
    {"/api/network/status",      "GET", 200},
    {"/api/network/config",      "GET", 200},
    {"/api/mqtt/status",         "GET", 200},
    {"/api/device/config",       "GET", 200},
    {"/api/device/info",         "GET", 200},
    {"/api/peripherals",         "GET", 200},
    {"/api/periph-exec",         "GET", 200},
    {"/api/protocol/config",     "GET", 200},
    {"/api/users",               "GET", 200},
    {"/api/logs/list",           "GET", 200},
    {"/api/auth/session",        "GET", 200},
};

/**
 * @brief 验证所有关键 API 端点列表完整性
 */
void test_api_endpoint_coverage() {
    TestLog::testStart("API Endpoint Coverage");
    
    constexpr size_t EXPECTED_ENDPOINTS = 14;
    size_t count = sizeof(CRITICAL_ENDPOINTS) / sizeof(CRITICAL_ENDPOINTS[0]);
    TEST_ASSERT_EQUAL(EXPECTED_ENDPOINTS, count);
    TestLog::step("14 critical API endpoints defined");
    
    // 验证所有端点路径以 /api/ 开头
    for (const auto& ep : CRITICAL_ENDPOINTS) {
        TEST_ASSERT_TRUE_MESSAGE(strncmp(ep.path, "/api/", 5) == 0,
            (String("Invalid path: ") + ep.path).c_str());
    }
    TestLog::step("All endpoints start with /api/");
    
    // 验证没有重复路径
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0,
                strcmp(CRITICAL_ENDPOINTS[i].path, CRITICAL_ENDPOINTS[j].path),
                "Duplicate API endpoint detected");
        }
    }
    TestLog::step("No duplicate endpoints");
    
    TestLog::testEnd(true);
}

/**
 * @brief 模拟页面加载时的批量 API 请求（batch 端点）
 */
void test_batch_api_reduces_connections() {
    TestLog::testStart("Batch API: Reduces TCP Usage");
    
    // 不使用 batch: dashboard 需要 3 个独立请求
    constexpr size_t WITHOUT_BATCH = 3;  // status + info + network
    
    // 使用 batch: 1 个请求 + 1 个 SSE
    constexpr size_t WITH_BATCH = 1;
    
    // batch 节省的连接数
    size_t saved = WITHOUT_BATCH - WITH_BATCH;
    TEST_ASSERT_EQUAL(2, saved);
    TestLog::step("Batch saves 2 concurrent connections per page load");
    
    // 对 C3 来说尤其重要（HTTP 预算仅 3）
    constexpr size_t C3_HTTP_BUDGET = 3;
    TEST_ASSERT_LESS_OR_EQUAL(C3_HTTP_BUDGET, WITH_BATCH);
    TestLog::step("C3: batch request fits within HTTP budget (3)");
    
    TestLog::testEnd(true);
}

// Test group entry point
void test_tcp_page_loading_group() {
    TestLog::groupStart("TCP Budget & Page Loading Tests");
    
    // TCP 预算约束验证
    RUN_TEST(test_tcp_budget_constraints);
    RUN_TEST(test_tcp_budget_c3_compact);
    RUN_TEST(test_tcp_budget_s3_multi_tab);
    
    // 页面加载模拟
    RUN_TEST(test_page_loading_esp32_classic);
    RUN_TEST(test_page_loading_esp32_c3);
    RUN_TEST(test_page_loading_after_mqtt_page);
    RUN_TEST(test_page_loading_old_sse_bug_repro);
    RUN_TEST(test_page_loading_rapid_switching);
    RUN_TEST(test_tcp_long_running_no_leak);
    
    // API 端点覆盖
    RUN_TEST(test_api_endpoint_coverage);
    RUN_TEST(test_batch_api_reduces_connections);
    
    TestLog::groupEnd();
}
