/**
 * @file test_pagination_fixes.cpp
 * @brief 分页修复验证测试 + 外设启用错误 + 缓冲区边界 + MutexGuard 超时
 *
 * 覆盖修复:
 * 1. PeripheralRouteHandler: degraded模式分页startIdx一致性
 * 2. PeriphExecRouteHandler: 低内存阈值3072 + MutexGuard 2s超时
 * 3. pendingBuf[130] 缓冲区边界保护
 * 4. 外设启用400错误详细信息(lastEnableError)
 * 5. PSRAM旁路: 文件API内存检查
 */

#include <unity.h>
#include <Arduino.h>
#include "mocks/MockPeripheral.h"
#include "mocks/MockHealthMonitor.h"
#include "helpers/TestConfig.h"
#include "helpers/TestAssertions.h"
#include "helpers/TestLogger.h"

#include <vector>
#include <algorithm>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

void test_pagination_fixes_group();

// ==============================================================
// 模拟分页逻辑（复现 PeripheralRouteHandler 修复）
// ==============================================================

struct PaginationParams {
    int page;
    int pageSize;
    bool degraded;  // shouldForceCompactList() 触发
};

struct PaginationResult {
    int startIdx;
    int endIdx;
    int effectivePageSize;
    int total;
};

// 复现修复后的分页逻辑
PaginationResult calculatePagination(const PaginationParams& params, int total) {
    PaginationResult result;
    result.total = total;

    int page = params.page;
    int pageSize = params.pageSize;
    if (page < 1) page = 1;
    if (pageSize < 1) pageSize = 1;
    if (pageSize > 20) pageSize = 20;

    // 修复核心: degraded 模式统一使用 effectivePageSize 作为分页步长，
    // startIdx/endIdx/返回的pageSize 三者一致，确保所有数据可达。
    int effectivePageSize = pageSize;
    if (params.degraded && effectivePageSize > 5) {
        effectivePageSize = 5;
    }

    result.effectivePageSize = effectivePageSize;
    result.startIdx = (page - 1) * effectivePageSize;  // 使用 effectivePageSize
    result.endIdx = (result.startIdx < total)
                    ? std::min(result.startIdx + effectivePageSize, total)
                    : result.startIdx;
    return result;
}

// 旧版有bug的分页逻辑（用于对比）
// 原始 bug: startIdx 使用客户端 pageSize，但 endIdx 限制为 effectivePageSize 条
PaginationResult calculatePaginationOldBug(const PaginationParams& params, int total) {
    PaginationResult result;
    result.total = total;

    int page = params.page;
    int pageSize = params.pageSize;
    if (page < 1) page = 1;
    if (pageSize < 1) pageSize = 1;
    if (pageSize > 20) pageSize = 20;

    int effectivePageSize = pageSize;
    if (params.degraded && effectivePageSize > 5) {
        effectivePageSize = 5;
    }

    result.effectivePageSize = effectivePageSize;
    // BUG: startIdx 使用客户端原始 pageSize，但只返回 effectivePageSize 条
    // 导致 page2 从startIdx=10开始，items 5-9 永远无法显示
    result.startIdx = (page - 1) * pageSize;
    result.endIdx = (result.startIdx < total)
                    ? std::min(result.startIdx + effectivePageSize, total)
                    : result.startIdx;
    return result;
}

// ==============================================================
// Test: 分页在 degraded 模式下 startIdx 保持客户端 pageSize 一致
// ==============================================================

void test_pagination_degraded_startIdx_consistency() {
    TestLog::testStart("Pagination: degraded mode uses effectivePageSize consistently");

    // 场景: 15 个外设，客户端 pageSize=10，degraded模式将 effectivePageSize 限制为5
    // 正确行为: page1: items 0-4, page2: items 5-9, page3: items 10-14
    // Bug行为(旧): startIdx=10 (page2*clientPageSize10) -> items 5-9 丢失

    PaginationParams params = {2, 10, true};  // page=2, pageSize=10, degraded=true
    int total = 15;

    PaginationResult fixed = calculatePagination(params, total);
    PaginationResult buggy = calculatePaginationOldBug(params, total);

    // 修复后: effectivePageSize=5, startIdx=(2-1)*5=5, endIdx=10（第2页看到item 5-9）
    TEST_ASSERT_EQUAL(5, fixed.startIdx);
    TEST_ASSERT_EQUAL(10, fixed.endIdx);
    TEST_ASSERT_EQUAL(5, fixed.effectivePageSize);
    TestLog::step("Fixed: page2 startIdx=5, endIdx=10 (items 5-9, no gap!)");

    // Bug版本(旧fix): startIdx=(2-1)*10=10，导致items 5-9永远无法显示
    TEST_ASSERT_EQUAL(10, buggy.startIdx);
    TEST_ASSERT_EQUAL(15, buggy.endIdx);
    TestLog::step("Bug (old fix): page2 startIdx=10 → items 5-9 LOST!");

    // 验证修复后第1页
    PaginationParams page1 = {1, 10, true};
    PaginationResult p1 = calculatePagination(page1, total);
    TEST_ASSERT_EQUAL(0, p1.startIdx);
    TEST_ASSERT_EQUAL(5, p1.endIdx);
    TestLog::step("Fixed page1: startIdx=0, endIdx=5 (items 0-4)");

    // 验证修复后第3页
    PaginationParams page3 = {3, 10, true};
    PaginationResult p3 = calculatePagination(page3, total);
    TEST_ASSERT_EQUAL(10, p3.startIdx);
    TEST_ASSERT_EQUAL(15, p3.endIdx);
    TestLog::step("Fixed page3: startIdx=10, endIdx=15 (items 10-14)");

    TestLog::testEnd(true);
}

// ==============================================================
// Test: 分页边界条件
// ==============================================================

void test_pagination_boundary_conditions() {
    TestLog::testStart("Pagination: Boundary Conditions");

    // 空列表
    PaginationResult r = calculatePagination({1, 10, false}, 0);
    TEST_ASSERT_EQUAL(0, r.startIdx);
    TEST_ASSERT_EQUAL(0, r.endIdx);
    TestLog::step("Empty list: startIdx=0, endIdx=0");

    // 请求超出范围的页
    r = calculatePagination({5, 10, false}, 15);
    TEST_ASSERT_EQUAL(40, r.startIdx);  // (5-1)*10 = 40
    TEST_ASSERT_EQUAL(40, r.endIdx);    // startIdx >= total, endIdx = startIdx
    TestLog::step("Out of range page: returns empty (startIdx=endIdx)");

    // 最后一页不满
    r = calculatePagination({2, 10, false}, 15);
    TEST_ASSERT_EQUAL(10, r.startIdx);
    TEST_ASSERT_EQUAL(15, r.endIdx);  // min(10+10, 15) = 15
    TestLog::step("Last partial page: endIdx=total (15)");

    // pageSize=1 逐条翻页
    r = calculatePagination({3, 1, false}, 10);
    TEST_ASSERT_EQUAL(2, r.startIdx);
    TEST_ASSERT_EQUAL(3, r.endIdx);
    TestLog::step("pageSize=1: item-by-item pagination");

    // 非 degraded 模式 pageSize 不被限制
    r = calculatePagination({1, 10, false}, 20);
    TEST_ASSERT_EQUAL(10, r.effectivePageSize);
    TEST_ASSERT_EQUAL(10, r.endIdx);
    TestLog::step("Normal mode: effectivePageSize=10 (not clamped)");

    TestLog::testEnd(true);
}

// ==============================================================
// Test: degraded 模式不会导致翻页内容重叠或跳过
// ==============================================================

void test_pagination_no_overlap_no_gap() {
    TestLog::testStart("Pagination: No overlap/gap across all pages (degraded)");

    int total = 23;
    int clientPageSize = 10;
    std::vector<int> allItems;

    // 遍历所有页面，收集返回的项索引
    for (int page = 1; page <= 10; page++) {
        PaginationResult r = calculatePagination({page, clientPageSize, true}, total);
        if (r.startIdx >= total) break;
        for (int i = r.startIdx; i < r.endIdx; i++) {
            allItems.push_back(i);
        }
    }

    // degraded 模式: effectivePageSize=5
    // page1: 0-4, page2: 5-9, page3: 10-14, page4: 15-19, page5: 20-22
    // 所有 23 个项都应被覆盖，无重叠无间隙
    TEST_ASSERT_EQUAL(total, (int)allItems.size());
    TestLog::step("Degraded mode covers ALL 23 items (no gap!)");

    // 验证无重复
    std::vector<int> sorted = allItems;
    std::sort(sorted.begin(), sorted.end());
    for (size_t i = 1; i < sorted.size(); i++) {
        TEST_ASSERT_TRUE_MESSAGE(sorted[i] > sorted[i-1], "Duplicate item detected!");
    }
    TestLog::step("No duplicate items across pages");

    // 验证连续性（无间隙）
    for (size_t i = 0; i < sorted.size(); i++) {
        TEST_ASSERT_EQUAL((int)i, sorted[i]);
    }
    TestLog::step("Items are contiguous: 0,1,2,...,22");

    // 非 degraded 模式也应覆盖所有项
    std::vector<int> normalItems;
    for (int page = 1; page <= 10; page++) {
        PaginationResult r = calculatePagination({page, clientPageSize, false}, total);
        if (r.startIdx >= total) break;
        for (int i = r.startIdx; i < r.endIdx; i++) {
            normalItems.push_back(i);
        }
    }
    TEST_ASSERT_EQUAL(total, (int)normalItems.size());
    TestLog::step("Normal mode covers all 23 items without gaps");

    TestLog::testEnd(true);
}

// ==============================================================
// Test: 低内存阈值 3072 for chunked streaming
// ==============================================================

void test_periph_exec_low_memory_threshold() {
    TestLog::testStart("PeriphExec: Low memory threshold 3072");

    // chunked 流式发送实际内存开销极小（~300B state + TCP buffer）
    // 阈值 3072 是最低运行空间保障
    constexpr uint32_t CHUNKED_THRESHOLD = 3072;

    // 堆 = 4096: 应允许请求
    ESP.setFreeHeap(4096);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= CHUNKED_THRESHOLD);
    TestLog::step("Heap=4096 >= 3072: request ALLOWED");

    // 堆 = 3072: 边界 - 应允许
    ESP.setFreeHeap(3072);
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= CHUNKED_THRESHOLD);
    TestLog::step("Heap=3072 (boundary): request ALLOWED");

    // 堆 = 3071: 低于阈值 - 应拒绝
    ESP.setFreeHeap(3071);
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= CHUNKED_THRESHOLD);
    TestLog::step("Heap=3071 < 3072: request REJECTED (503)");

    // 旧阈值 6144 对比: 堆 5000 本应通过但被旧逻辑误拒
    ESP.setFreeHeap(5000);
    constexpr uint32_t OLD_THRESHOLD = 6144;
    TEST_ASSERT_FALSE(ESP.getFreeHeap() >= OLD_THRESHOLD);  // 旧阈值会拒绝
    TEST_ASSERT_TRUE(ESP.getFreeHeap() >= CHUNKED_THRESHOLD);  // 新阈值允许
    TestLog::step("Heap=5000: old(6144) rejects, new(3072) allows");

    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// ==============================================================
// Test: MutexGuard 超时行为模拟
// ==============================================================

// 模拟 MutexGuard 的超时逻辑
class MockMutexGuard {
public:
    MockMutexGuard(std::timed_mutex& mutex, int timeoutMs)
        : _mutex(mutex), _locked(false)
    {
        _locked = _mutex.try_lock_for(std::chrono::milliseconds(timeoutMs));
    }

    ~MockMutexGuard() {
        if (_locked) {
            _mutex.unlock();
        }
    }

    bool isLocked() const { return _locked; }

private:
    std::timed_mutex& _mutex;
    bool _locked;
};

void test_mutex_guard_timeout_behavior() {
    TestLog::testStart("MutexGuard: Timeout prevents infinite wait");

    std::timed_mutex testMutex;
    std::atomic<bool> lockAcquired(false);
    std::atomic<bool> threadDone(false);

    // 先在主线程获取锁
    testMutex.lock();
    TestLog::step("Main thread holds the mutex");

    // 在另一个线程中尝试用超时获取锁
    std::thread worker([&]() {
        MockMutexGuard guard(testMutex, 200);  // 200ms 超时
        lockAcquired.store(guard.isLocked());
        threadDone.store(true);
    });

    // 等待工作线程完成（应该在 ~200ms 后超时返回）
    auto start = std::chrono::steady_clock::now();
    while (!threadDone.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 2000) {
            break;  // 安全退出防止死锁
        }
    }

    // 验证: 工作线程应该在超时后返回，且未获得锁
    TEST_ASSERT_TRUE(threadDone.load());
    TEST_ASSERT_FALSE(lockAcquired.load());
    TestLog::step("Worker thread timed out (200ms) without getting lock");

    // 释放锁后重试应成功
    testMutex.unlock();
    TestLog::step("Main thread released mutex");

    {
        MockMutexGuard guard(testMutex, 200);
        TEST_ASSERT_TRUE(guard.isLocked());
        TestLog::step("Lock acquired after release (no contention)");
    }

    worker.join();
    TestLog::testEnd(true);
}

// ==============================================================
// Test: MutexGuard 超时返回 503 错误场景
// ==============================================================

void test_mutex_guard_503_response() {
    TestLog::testStart("MutexGuard: 503 response on lock timeout");

    // 模拟服务端处理逻辑
    struct MockResponse {
        int statusCode = 0;
        String body;
        int retryAfter = 0;
    };

    auto handleRequest = [](bool lockAcquired) -> MockResponse {
        MockResponse resp;
        if (!lockAcquired) {
            resp.statusCode = 503;
            resp.body = "Service busy (rules locked), retry in 2s";
            resp.retryAfter = 2;
            return resp;
        }
        resp.statusCode = 200;
        resp.body = "{\"items\":[],\"total\":0}";
        return resp;
    };

    // 锁获取失败 → 503
    MockResponse failResp = handleRequest(false);
    TEST_ASSERT_EQUAL(503, failResp.statusCode);
    TEST_ASSERT_EQUAL(2, failResp.retryAfter);
    TEST_ASSERT_TRUE(failResp.body.indexOf("retry") >= 0);
    TestLog::step("Lock timeout → 503 with retry hint");

    // 锁获取成功 → 200
    MockResponse okResp = handleRequest(true);
    TEST_ASSERT_EQUAL(200, okResp.statusCode);
    TestLog::step("Lock acquired → 200 OK");

    TestLog::testEnd(true);
}

// ==============================================================
// Test: 缓冲区边界保护 (pendingBuf[130])
// ==============================================================

void test_buffer_boundary_protection() {
    TestLog::testStart("Buffer: pendingBuf[130] boundary protection");

    constexpr int BUF_SIZE = 130;
    char pendingBuf[BUF_SIZE];
    memset(pendingBuf, 0, BUF_SIZE);

    // 模拟安全写入（带边界检查）
    auto safeAppend = [&](const char* data, int& writePos) -> bool {
        int dataLen = strlen(data);
        if (writePos + dataLen >= BUF_SIZE - 1) {
            // 缓冲区满，截断或刷新
            return false;
        }
        memcpy(pendingBuf + writePos, data, dataLen);
        writePos += dataLen;
        pendingBuf[writePos] = '\0';
        return true;
    };

    int pos = 0;

    // 正常写入
    TEST_ASSERT_TRUE(safeAppend("Hello", pos));
    TEST_ASSERT_EQUAL(5, pos);
    TestLog::step("Normal write: 5 bytes OK");

    // 大量写入接近边界
    char bigData[100];
    memset(bigData, 'A', 99);
    bigData[99] = '\0';
    TEST_ASSERT_TRUE(safeAppend(bigData, pos));
    TEST_ASSERT_EQUAL(104, pos);
    TestLog::step("Near-boundary write: 104/130 bytes");

    // 再写入超出边界的数据 - 应被拒绝
    char overflow[30];
    memset(overflow, 'B', 29);
    overflow[29] = '\0';
    TEST_ASSERT_FALSE(safeAppend(overflow, pos));
    TEST_ASSERT_EQUAL(104, pos);  // 写位置不变
    TestLog::step("Overflow attempt rejected: pos stays at 104");

    // 验证缓冲区没有溢出
    TEST_ASSERT_EQUAL('\0', pendingBuf[pos]);
    TEST_ASSERT_EQUAL(104, (int)strlen(pendingBuf));
    TestLog::step("Buffer integrity verified (no overflow)");

    // 刚好填满到129字节（最大安全位置）
    pos = 0;
    memset(pendingBuf, 0, BUF_SIZE);
    char maxData[BUF_SIZE - 1];
    memset(maxData, 'X', BUF_SIZE - 2);
    maxData[BUF_SIZE - 2] = '\0';
    TEST_ASSERT_TRUE(safeAppend(maxData, pos));
    TEST_ASSERT_EQUAL(BUF_SIZE - 2, pos);  // 128
    TestLog::step("Max fill to 128 bytes (leaves room for null terminator)");

    // 再写1个字符都不行
    TEST_ASSERT_FALSE(safeAppend("Z", pos));
    TestLog::step("Even 1 more byte rejected at boundary");

    TestLog::testEnd(true);
}

// ==============================================================
// Test: 外设启用错误详细信息 (lastEnableError)
// ==============================================================

// 模拟外设启用错误跟踪
class MockPeripheralWithError {
public:
    struct EnableResult {
        bool success;
        String error;
        int httpStatus;
    };

    EnableResult enable(const String& periphId) {
        EnableResult result;

        if (periphId.isEmpty()) {
            result.success = false;
            result.error = "Peripheral ID is empty";
            result.httpStatus = 400;
            _lastEnableError = result.error;
            return result;
        }

        // 模拟 pin 冲突
        if (periphId == "conflict_pin") {
            result.success = false;
            result.error = "Pin 2 already used by peripheral 'led_output'";
            result.httpStatus = 400;
            _lastEnableError = result.error;
            return result;
        }

        // 模拟初始化失败
        if (periphId == "init_fail") {
            result.success = false;
            result.error = "GPIO initialization failed: pin not available";
            result.httpStatus = 500;
            _lastEnableError = result.error;
            return result;
        }

        result.success = true;
        result.error = "";
        result.httpStatus = 200;
        _lastEnableError = "";
        return result;
    }

    String getLastEnableError() const { return _lastEnableError; }
    void clearError() { _lastEnableError = ""; }

private:
    String _lastEnableError;
};

void test_peripheral_enable_error_detail() {
    TestLog::testStart("Peripheral: Enable error detail (lastEnableError)");

    MockPeripheralWithError pm;

    // 正常启用
    auto result = pm.enable("led_output");
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL(200, result.httpStatus);
    TEST_ASSERT_TRUE(pm.getLastEnableError().isEmpty());
    TestLog::step("Normal enable: success, no error");

    // 空 ID 错误
    result = pm.enable("");
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_EQUAL(400, result.httpStatus);
    TEST_ASSERT_TRUE(result.error.indexOf("empty") >= 0);
    TestLog::step("Empty ID: 400 with descriptive error");

    // Pin 冲突
    result = pm.enable("conflict_pin");
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_EQUAL(400, result.httpStatus);
    TEST_ASSERT_TRUE(result.error.indexOf("Pin") >= 0);
    TEST_ASSERT_TRUE(result.error.indexOf("already used") >= 0);
    TestLog::step("Pin conflict: 400 with 'Pin X already used by Y'");

    // 初始化失败
    result = pm.enable("init_fail");
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_EQUAL(500, result.httpStatus);
    TEST_ASSERT_TRUE(result.error.indexOf("initialization failed") >= 0);
    TestLog::step("Init fail: 500 with GPIO error detail");

    // lastEnableError 保持最后一个错误
    TEST_ASSERT_TRUE(pm.getLastEnableError().indexOf("initialization") >= 0);
    TestLog::step("lastEnableError preserves last error");

    TestLog::testEnd(true);
}

// ==============================================================
// Test: PSRAM 旁路 - 文件 API 内存检查
// ==============================================================

void test_psram_bypass_file_api() {
    TestLog::testStart("PSRAM Bypass: File API memory check");

    // 模拟文件 API 在 PSRAM 不可用时的内存保护
    constexpr uint32_t FILE_API_HEAP_THRESHOLD = 8192;
    constexpr uint32_t PSRAM_MIN_FREE = 65536;  // 64KB

    // 场景1: PSRAM 充足 → 允许大文件操作
    ESP.setPsramSize(8 * 1024 * 1024);
    ESP.setFreePsram(4 * 1024 * 1024);
    ESP.setFreeHeap(50000);

    bool psramAvailable = (ESP.getFreePsram() > PSRAM_MIN_FREE);
    TEST_ASSERT_TRUE(psramAvailable);
    TestLog::step("PSRAM 4MB free > 64KB threshold: file ops allowed");

    // 场景2: 无 PSRAM，但堆充足 → 允许
    ESP.setPsramSize(0);
    ESP.setFreePsram(0);
    ESP.setFreeHeap(50000);

    psramAvailable = (ESP.getFreePsram() > PSRAM_MIN_FREE);
    bool heapSufficient = (ESP.getFreeHeap() >= FILE_API_HEAP_THRESHOLD);
    bool fileOpAllowed = psramAvailable || heapSufficient;
    TEST_ASSERT_TRUE(fileOpAllowed);
    TestLog::step("No PSRAM but heap=50KB: file ops allowed via DRAM");

    // 场景3: 无 PSRAM 且堆不足 → 拒绝
    ESP.setFreeHeap(5000);
    heapSufficient = (ESP.getFreeHeap() >= FILE_API_HEAP_THRESHOLD);
    fileOpAllowed = (ESP.getFreePsram() > PSRAM_MIN_FREE) || heapSufficient;
    TEST_ASSERT_FALSE(fileOpAllowed);
    TestLog::step("No PSRAM + heap=5KB: file ops REJECTED (503)");

    // 场景4: PSRAM 极低 + 堆极低 → 拒绝
    ESP.setPsramSize(8 * 1024 * 1024);
    ESP.setFreePsram(30000);  // < 64KB
    ESP.setFreeHeap(4000);    // < 8KB
    psramAvailable = (ESP.getFreePsram() > PSRAM_MIN_FREE);
    heapSufficient = (ESP.getFreeHeap() >= FILE_API_HEAP_THRESHOLD);
    fileOpAllowed = psramAvailable || heapSufficient;
    TEST_ASSERT_FALSE(fileOpAllowed);
    TestLog::step("PSRAM low + heap low: file ops REJECTED");

    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// ==============================================================
// Test: 分页参数非法值保护
// ==============================================================

void test_pagination_invalid_params() {
    TestLog::testStart("Pagination: Invalid parameter protection");

    // page < 1 → 自动修正为 1
    PaginationResult r = calculatePagination({0, 10, false}, 20);
    TEST_ASSERT_EQUAL(0, r.startIdx);  // page被修正为1: (1-1)*10 = 0
    TestLog::step("page=0 → corrected to page=1, startIdx=0");

    r = calculatePagination({-5, 10, false}, 20);
    TEST_ASSERT_EQUAL(0, r.startIdx);
    TestLog::step("page=-5 → corrected to page=1");

    // pageSize < 1 → 自动修正为 1
    r = calculatePagination({1, 0, false}, 20);
    TEST_ASSERT_EQUAL(0, r.startIdx);
    TEST_ASSERT_EQUAL(1, r.endIdx);  // effectivePageSize=1
    TestLog::step("pageSize=0 → corrected to 1");

    // pageSize > 20 → 上限限制
    r = calculatePagination({1, 100, false}, 50);
    TEST_ASSERT_EQUAL(20, r.effectivePageSize);
    TEST_ASSERT_EQUAL(20, r.endIdx);
    TestLog::step("pageSize=100 → clamped to 20");

    TestLog::testEnd(true);
}

// ==============================================================
// Smoke Test: degraded 模式触发条件
// ==============================================================

void test_smoke_degraded_mode_trigger() {
    TestLog::testStart("Smoke: Degraded mode trigger conditions");

    // shouldForceCompactList() 逻辑: heap < 16KB || maxAlloc < 8192
    constexpr uint32_t DEGRADED_HEAP_THRESHOLD = 16384;

    // 正常: 堆 80KB → 非 degraded
    ESP.setFreeHeap(80000);
    bool degraded = (ESP.getFreeHeap() < DEGRADED_HEAP_THRESHOLD);
    TEST_ASSERT_FALSE(degraded);
    TestLog::step("Heap=80KB: NOT degraded");

    // 堆 20KB → 非 degraded（边界之上）
    ESP.setFreeHeap(20000);
    degraded = (ESP.getFreeHeap() < DEGRADED_HEAP_THRESHOLD);
    TEST_ASSERT_FALSE(degraded);
    TestLog::step("Heap=20KB: NOT degraded");

    // 堆 16KB → 非 degraded（刚好等于阈值）
    ESP.setFreeHeap(16384);
    degraded = (ESP.getFreeHeap() < DEGRADED_HEAP_THRESHOLD);
    TEST_ASSERT_FALSE(degraded);
    TestLog::step("Heap=16384: NOT degraded (boundary)");

    // 堆 15KB → degraded
    ESP.setFreeHeap(15000);
    degraded = (ESP.getFreeHeap() < DEGRADED_HEAP_THRESHOLD);
    TEST_ASSERT_TRUE(degraded);
    TestLog::step("Heap=15KB: DEGRADED mode active");

    // 堆 8KB → 严重 degraded
    ESP.setFreeHeap(8000);
    degraded = (ESP.getFreeHeap() < DEGRADED_HEAP_THRESHOLD);
    TEST_ASSERT_TRUE(degraded);
    TestLog::step("Heap=8KB: DEGRADED (severe)");

    // maxAlloc 检查
    // MockESP getMaxAllocHeap() = getFreeHeap() / 2
    ESP.setFreeHeap(20000);  // maxAlloc = 10000 > 8192, heap > 16KB
    uint32_t maxAlloc = ESP.getMaxAllocHeap();
    bool allocDegraded = (maxAlloc < 8192);
    TEST_ASSERT_FALSE(allocDegraded);
    TestLog::step("maxAlloc=10000 > 8192: NOT degraded by alloc");

    ESP.setFreeHeap(14000);  // maxAlloc = 7000 < 8192
    maxAlloc = ESP.getMaxAllocHeap();
    allocDegraded = (maxAlloc < 8192);
    TEST_ASSERT_TRUE(allocDegraded);
    TestLog::step("maxAlloc=7000 < 8192: DEGRADED by fragmentation");

    ESP.resetHeapOverride();
    TestLog::testEnd(true);
}

// ==============================================================
// Smoke Test: 分页+外设管理完整工作流
// ==============================================================

void test_smoke_pagination_full_workflow() {
    TestLog::testStart("Smoke: Pagination full workflow (add→list→paginate)");

    MockPeripheralManager& pm = MockPeripheralManager::getInstance();
    pm.initialize();
    pm.clearAll();

    // 添加 15 个外设
    for (int i = 0; i < 15; i++) {
        PeripheralConfig cfg;
        cfg.id = "periph_" + String(i);
        cfg.name = "Peripheral " + String(i);
        cfg.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
        cfg.pin = i + 2;
        cfg.enabled = (i < 12);  // 前12个启用
        TEST_ASSERT_TRUE(pm.addPeripheral(cfg));
    }
    TEST_ASSERT_EQUAL(15, pm.getPeripheralCount());
    TestLog::step("15 peripherals added");

    // 模拟分页请求
    std::vector<String> ids = pm.getPeripheralIds();
    int total = ids.size();
    TEST_ASSERT_EQUAL(15, total);

    // 第1页 (normal mode)
    PaginationResult p1 = calculatePagination({1, 10, false}, total);
    TEST_ASSERT_EQUAL(0, p1.startIdx);
    TEST_ASSERT_EQUAL(10, p1.endIdx);
    TestLog::step("Page 1 (normal): items 0-9");

    // 第2页 (normal mode)
    PaginationResult p2 = calculatePagination({2, 10, false}, total);
    TEST_ASSERT_EQUAL(10, p2.startIdx);
    TEST_ASSERT_EQUAL(15, p2.endIdx);
    TestLog::step("Page 2 (normal): items 10-14");

    // 第1页 (degraded mode) - effectivePageSize=5
    PaginationResult dp1 = calculatePagination({1, 10, true}, total);
    TEST_ASSERT_EQUAL(0, dp1.startIdx);
    TEST_ASSERT_EQUAL(5, dp1.endIdx);
    TestLog::step("Page 1 (degraded): items 0-4");

    // 第2页 (degraded mode) - 关键修复验证
    PaginationResult dp2 = calculatePagination({2, 10, true}, total);
    TEST_ASSERT_EQUAL(5, dp2.startIdx);   // 基于 effectivePageSize=5
    TEST_ASSERT_EQUAL(10, dp2.endIdx);
    TestLog::step("Page 2 (degraded): items 5-9 (no gap!)");

    // 第3页 (degraded mode)
    PaginationResult dp3 = calculatePagination({3, 10, true}, total);
    TEST_ASSERT_EQUAL(10, dp3.startIdx);
    TEST_ASSERT_EQUAL(15, dp3.endIdx);
    TestLog::step("Page 3 (degraded): items 10-14");

    pm.clearAll();
    TestLog::testEnd(true);
}

// ==============================================================
// Smoke Test: 外设执行规则分页
// ==============================================================

void test_smoke_periph_exec_pagination() {
    TestLog::testStart("Smoke: PeriphExec rule pagination");

    MockPeriphExecManager& em = MockPeriphExecManager::getInstance();
    em.initialize();
    em.clearAll();

    // 添加 25 条规则
    for (int i = 0; i < 25; i++) {
        PeriphExecRule rule;
        rule.id = "rule_" + String(i);
        rule.name = "Rule " + String(i);
        rule.targetPeriphId = "periph_" + String(i % 5);
        rule.actionType = ActionType::SET_HIGH;
        rule.enabled = (i < 20);
        rule.priority = i;
        TEST_ASSERT_TRUE(em.addRule(rule));
    }
    TEST_ASSERT_EQUAL(25, em.getRuleCount());
    TestLog::step("25 rules added");

    int total = em.getRuleCount();

    // page=1, pageSize=10
    PaginationResult p1 = calculatePagination({1, 10, false}, total);
    TEST_ASSERT_EQUAL(0, p1.startIdx);
    TEST_ASSERT_EQUAL(10, p1.endIdx);
    int p1Count = p1.endIdx - p1.startIdx;
    TEST_ASSERT_EQUAL(10, p1Count);
    TestLog::step("PeriphExec Page 1: 10 rules");

    // page=2, pageSize=10
    PaginationResult p2 = calculatePagination({2, 10, false}, total);
    TEST_ASSERT_EQUAL(10, p2.startIdx);
    TEST_ASSERT_EQUAL(20, p2.endIdx);
    TestLog::step("PeriphExec Page 2: 10 rules");

    // page=3, pageSize=10
    PaginationResult p3 = calculatePagination({3, 10, false}, total);
    TEST_ASSERT_EQUAL(20, p3.startIdx);
    TEST_ASSERT_EQUAL(25, p3.endIdx);
    int p3Count = p3.endIdx - p3.startIdx;
    TEST_ASSERT_EQUAL(5, p3Count);
    TestLog::step("PeriphExec Page 3: 5 rules (last page)");

    // page=4 → 空
    PaginationResult p4 = calculatePagination({4, 10, false}, total);
    TEST_ASSERT_EQUAL(30, p4.startIdx);  // (4-1)*10=30 > 25
    TEST_ASSERT_EQUAL(30, p4.endIdx);    // empty
    TestLog::step("PeriphExec Page 4: empty (beyond total)");

    em.clearAll();
    TestLog::testEnd(true);
}

// ==============================================================
// Test group entry point
// ==============================================================

void test_pagination_fixes_group() {
    UNITY_BEGIN();

    TestLog::groupStart("Pagination Fixes & Smoke Tests");

    // 核心分页修复验证
    RUN_TEST(test_pagination_degraded_startIdx_consistency);
    RUN_TEST(test_pagination_boundary_conditions);
    RUN_TEST(test_pagination_no_overlap_no_gap);
    RUN_TEST(test_pagination_invalid_params);

    // 低内存阈值修复
    RUN_TEST(test_periph_exec_low_memory_threshold);

    // MutexGuard 超时修复
    RUN_TEST(test_mutex_guard_timeout_behavior);
    RUN_TEST(test_mutex_guard_503_response);

    // 缓冲区边界保护
    RUN_TEST(test_buffer_boundary_protection);

    // 外设启用错误详细信息
    RUN_TEST(test_peripheral_enable_error_detail);

    // PSRAM 旁路逻辑
    RUN_TEST(test_psram_bypass_file_api);

    // Smoke Tests
    RUN_TEST(test_smoke_degraded_mode_trigger);
    RUN_TEST(test_smoke_pagination_full_workflow);
    RUN_TEST(test_smoke_periph_exec_pagination);

    TestLog::groupEnd();

    UNITY_END();
}
