#pragma once

/**
 * @file StringBufferPool.h
 * @brief 固定大小缓冲池，避免频繁 String new/delete 碎片化
 *
 * 用途：
 * - MQTT 上报数据序列化临时缓冲
 * - 日志格式化临时缓冲
 * - Modbus 轮询结果临时存储
 *
 * 设计：
 * - 编译期确定缓冲大小和池数量
 * - acquire() 获取空闲槽位，release() 归还
 * - 无动态分配，零碎片化
 * - 线程安全：中断级别使用（MQTT callback）需额外加锁
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {

template<size_t BufSize = 256, uint8_t PoolCount = 4>
class StringBufferPool {
public:
    static constexpr size_t BUFFER_SIZE = BufSize;
    static constexpr uint8_t POOL_COUNT = PoolCount;

    struct Slot {
        char data[BufSize];
        bool inUse;

        Slot() : inUse(false) {
            memset(data, 0, sizeof(data));
        }
    };

    StringBufferPool() = default;

    // 禁止拷贝
    StringBufferPool(const StringBufferPool&) = delete;
    StringBufferPool& operator=(const StringBufferPool&) = delete;

    /**
     * @brief 获取一个空闲缓冲区
     * @return 指向空闲缓冲区的指针，池满时返回 nullptr
     */
    char* acquire() {
        for (uint8_t i = 0; i < PoolCount; i++) {
            if (!_slots[i].inUse) {
                _slots[i].inUse = true;
                _slots[i].data[0] = '\0';
                _acquireCount++;
                return _slots[i].data;
            }
        }
        _exhaustCount++;
        return nullptr;
    }

    /**
     * @brief 归还之前获取的缓冲区
     * @param ptr 之前 acquire() 返回的指针
     */
    void release(char* ptr) {
        if (!ptr) return;
        for (uint8_t i = 0; i < PoolCount; i++) {
            if (_slots[i].data == ptr) {
                _slots[i].inUse = false;
                _slots[i].data[0] = '\0';
                _releaseCount++;
                return;
            }
        }
        // ptr 不属于本池，忽略
    }

    /**
     * @brief 检查指针是否属于本池
     */
    bool owns(const char* ptr) const {
        if (!ptr) return false;
        for (uint8_t i = 0; i < PoolCount; i++) {
            if (_slots[i].data == ptr) return true;
        }
        return false;
    }

    /**
     * @brief 获取当前已占用的槽位数
     */
    uint8_t usedCount() const {
        uint8_t count = 0;
        for (uint8_t i = 0; i < PoolCount; i++) {
            if (_slots[i].inUse) count++;
        }
        return count;
    }

    /**
     * @brief 获取池总容量
     */
    constexpr uint8_t capacity() const { return PoolCount; }

    /**
     * @brief 强制释放所有槽位（仅用于系统恢复/重启场景）
     */
    void reset() {
        for (uint8_t i = 0; i < PoolCount; i++) {
            _slots[i].inUse = false;
            _slots[i].data[0] = '\0';
        }
    }

    // 统计信息
    uint32_t acquireTotal() const { return _acquireCount; }
    uint32_t releaseTotal() const { return _releaseCount; }
    uint32_t exhaustTotal() const { return _exhaustCount; }

private:
    Slot _slots[PoolCount];
    uint32_t _acquireCount = 0;
    uint32_t _releaseCount = 0;
    uint32_t _exhaustCount = 0;
};

} // namespace FastBee
