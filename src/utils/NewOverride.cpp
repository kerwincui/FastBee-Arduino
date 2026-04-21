// 全局 operator new 重写
// ESP32 的 libstdc++ 默认在内存分配失败时抛出 std::bad_alloc，
// 但嵌入式系统中很多代码（如 AsyncTCP 的 cbuf::resizeAdd）
// 通过 if(newbuf) 检查 nullptr 来优雅处理 OOM，
// 却没有 catch(std::bad_alloc) 的异常处理路径。
// 因此重写为 malloc()，分配失败时返回 nullptr 而不是抛异常。
//
// 重要：不能加 -fno-exceptions 编译标志！
// ESP32 Arduino 框架的预编译库（FS、NVS 等）使用 std::make_shared，
// 内部依赖异常展开机制。-fno-exceptions 会导致库异常无法展开→abort()。

#include <cstdlib>
#include <new>
#include <esp_log.h>

static const char* TAG = "NewOverride";

void* operator new(size_t size) {
    if (size == 0) size = 1;  // C++ 标准：new(0) 必须返回非 nullptr
    void* p = malloc(size);
    if (!p) {
        // OOM：记录日志但不抛异常，让调用者通过 nullptr 检查处理
        ESP_LOGE(TAG, "operator new(%u) failed", (unsigned)size);
    }
    return p;
}

void* operator new[](size_t size) {
    if (size == 0) size = 1;
    void* p = malloc(size);
    if (!p) {
        ESP_LOGE(TAG, "operator new[](%u) failed", (unsigned)size);
    }
    return p;
}

void* operator new(size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    return malloc(size);
}

void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    if (size == 0) size = 1;
    return malloc(size);
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
