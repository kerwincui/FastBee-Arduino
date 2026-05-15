// 全局 operator new 重写
// ESP32 的 libstdc++ 默认在内存分配失败时抛出 std::bad_alloc。
// 本项目嵌入式环境下，异常展开不总是可靠，因此重写分配器。
//
// === 语义契约（遵循 C++ 标准）===
// 1) 裸 new T(...) / new T[N]（标准版本）
//    分配失败 → ets_printf 记录 → abort() 立即复位
//    原因：C++ 标准规定裸 new 失败必须抛 bad_alloc 或终止，
//    绝不能返回 nullptr。若返回 nullptr，编译器仍会在
//    nullptr 地址上执行构造函数，导致 StoreProhibited 崩溃
//    （典型场景：ESPAsyncWebServer 的 beginChunkedResponse
//    内部 `new AsyncChunkedResponse(...)` 无 null 检查）。
//
// 2) new (std::nothrow) T(...) / new (std::nothrow) T[N]
//    分配失败 → 返回 nullptr（不构造），由调用方检查
//    典型用户：AsyncTCP 的 cbuf::resizeAdd、用户显式选择的降级路径。
//
// 重要：不能加 -fno-exceptions 编译标志！
// ESP32 Arduino 框架的预编译库（FS、NVS 等）使用 std::make_shared，
// 内部依赖异常展开机制。-fno-exceptions 会导致库异常无法展开→abort()。
//
// 重要：OOM 路径上绝对不能用 ESP_LOGE / printf 等会再次分配内存的写日志方式，
// 否则 newlib locks lazy-init 递归分配 → 再次 OOM → abort（曾见于 logo.png 404 路径下崩溃）。
// ets_printf 是 ROM 函数，不走 newlib / 不加锁 / 不分配内存，OOM 路径下绝对安全。

#include <cstdlib>
#include <new>

extern "C" int ets_printf(const char* fmt, ...);
extern "C" void abort(void) __attribute__((noreturn));

void* operator new(size_t size) {
    if (size == 0) size = 1;  // C++ 标准：new(0) 必须返回非 nullptr
    void* p = malloc(size);
    if (!p) {
        // OOM：用 ROM 的 ets_printf，不依赖堆和锁
        ets_printf("[NewOverride] operator new(%u) FATAL, abort\n", (unsigned)size);
        abort();  // 触发 panic handler → esp_restart，避免在 nullptr 上构造
    }
    return p;
}

void* operator new[](size_t size) {
    if (size == 0) size = 1;
    void* p = malloc(size);
    if (!p) {
        ets_printf("[NewOverride] operator new[](%u) FATAL, abort\n", (unsigned)size);
        abort();
    }
    return p;
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    void* p = malloc(size);
    if (!p) {
        ets_printf("[NewOverride] nothrow new(%u) failed\n", (unsigned)size);
    }
    return p;
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    void* p = malloc(size);
    if (!p) {
        ets_printf("[NewOverride] nothrow new[](%u) failed\n", (unsigned)size);
    }
    return p;
}

void operator delete(void* ptr) noexcept {
    free(ptr);
}

void operator delete[](void* ptr) noexcept {
    free(ptr);
}

void operator delete(void* ptr, size_t) noexcept {
    free(ptr);
}

void operator delete[](void* ptr, size_t) noexcept {
    free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    free(ptr);
}
