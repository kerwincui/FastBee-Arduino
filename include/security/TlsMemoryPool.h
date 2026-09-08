/**
 * @file TlsMemoryPool.h
 * @brief Compatibility facade for the removed TLS memory pool prototype.
 * @note The firmware uses the platform TLS allocator directly. This header is
 *       retained for source compatibility with external integrations; it is
 *       not part of the boot path and does not provide pooled allocation.
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace FastBee {

class TlsMemoryPool {
public:
    static TlsMemoryPool& instance() {
        static TlsMemoryPool inst;
        return inst;
    }
    
    bool initialize() {
        // Compatibility-only state; no pool is allocated.
        initialized_ = true;
        return true;
    }
    
    bool isInitialized() const {
        return initialized_;
    }
    
    size_t getPoolSize() const {
        return 0;
    }
    
    size_t getUsedSize() const {
        return 0;
    }
    
    size_t getFreeSize() const {
        return 0;
    }
    
    void* allocate(size_t size) {
        // The platform TLS implementation owns allocation and falls back to
        // its regular heap path when no dedicated pool is configured.
        (void)size;
        return nullptr;
    }
    
    void deallocate(void* ptr) {
        (void)ptr;
    }
    
    void reset() {
        initialized_ = false;
    }
    
private:
    TlsMemoryPool() : initialized_(false) {}
    ~TlsMemoryPool() = default;
    TlsMemoryPool(const TlsMemoryPool&) = delete;
    TlsMemoryPool& operator=(const TlsMemoryPool&) = delete;
    
    bool initialized_;
};

} // namespace FastBee
