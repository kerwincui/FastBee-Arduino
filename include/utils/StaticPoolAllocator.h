#ifndef FB_STATIC_POOL_ALLOCATOR_H
#define FB_STATIC_POOL_ALLOCATOR_H

/**
 * @description: 静态固定块内存池 + STL Allocator 适配器
 *               用于 std::map / std::set 等节点型容器的堆碎片治理
 *
 * 设计目标（对应 FastBee 内存优化 T3）：
 *  - 启动时一次性预留固定大小的内存池（DRAM），为 std::map 节点提供"不会被操作系统/其它模块复用"的稳定区域
 *  - 运行期增删节点仅在池内 freelist 中串链，不触发 malloc/free，**根治堆碎片**
 *  - 池耗尽时优雅回退到 ::malloc，**保证功能不降级**
 *
 * 关键约束：
 *  - STL Allocator 必须支持 rebind（std::map 内部会 rebind 到 _Rb_tree_node<value_type>）
 *  - STL 节点只会以 n==1 的 allocate 申请；n>1 的情况（如 std::vector）回退到 malloc
 *  - 池为每种 (BlockSize, BlockCount) 组合使用一个 Meyers 单例 slab，跨线程安全需外层 mutex 保证
 *
 * 内存占用：
 *  - 单池 RAM = BlockSize * BlockCount + 少量元数据
 *  - 例：512 * 16 = 8KB，适合 peripherals/rules 两个 map 共享
 *
 * @author: kerwincui
 * @copyright: FastBee All rights reserved.
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <type_traits>

namespace FastBee {

// ── 固定块 Slab Pool ────────────────────────────────────────────────────────
// 提供定长块的 O(1) 分配/释放，内部使用 freelist 侵入式链表
template <size_t BlockSize, size_t BlockCount>
class StaticSlabPool {
    static_assert(BlockSize >= sizeof(void*), "BlockSize must hold a pointer");
    static_assert(BlockCount > 0, "BlockCount must be positive");

public:
    StaticSlabPool() noexcept { reset(); }

    // 从池中申请一个块；池空则 fallback 到 malloc
    void* allocate() noexcept {
        if (_freeHead) {
            Node* n = _freeHead;
            _freeHead = n->next;
            ++_usedCount;
            if (_usedCount > _peakUsed) _peakUsed = _usedCount;
            return n;
        }
        ++_overflowCount;
        return ::malloc(BlockSize);
    }

    // 释放；若指针落在池区间内则归还到 freelist，否则 free
    void deallocate(void* p) noexcept {
        if (!p) return;
        if (isInPool(p)) {
            Node* n = static_cast<Node*>(p);
            n->next = _freeHead;
            _freeHead = n;
            if (_usedCount > 0) --_usedCount;
        } else {
            ::free(p);
        }
    }

    size_t usedCount() const noexcept { return _usedCount; }
    size_t peakUsed() const noexcept  { return _peakUsed; }
    size_t overflowCount() const noexcept { return _overflowCount; }
    static constexpr size_t capacity() noexcept { return BlockCount; }
    static constexpr size_t blockSize() noexcept { return BlockSize; }
    static constexpr size_t totalBytes() noexcept { return BlockSize * BlockCount; }

private:
    union Node {
        Node* next;
        alignas(alignof(std::max_align_t)) uint8_t raw[BlockSize];
    };

    Node _pool[BlockCount];
    Node* _freeHead = nullptr;
    size_t _usedCount = 0;
    size_t _peakUsed = 0;
    size_t _overflowCount = 0;

    bool isInPool(void* p) const noexcept {
        return p >= static_cast<const void*>(&_pool[0])
            && p <  static_cast<const void*>(&_pool[BlockCount]);
    }

    void reset() noexcept {
        _freeHead = nullptr;
        // 反向构建 freelist，使首次分配从 pool[0] 开始（cache-friendly）
        for (size_t i = BlockCount; i > 0; --i) {
            _pool[i - 1].next = _freeHead;
            _freeHead = &_pool[i - 1];
        }
    }
};

// 跨 T 共享的池访问函数：以 (BlockSize, BlockCount) 为唯一键
// 避免多个 std::map<K,V> 同参数多池冗余（修复原设计中 Meyers 单例在模板内导致的 T 隔离问题）
template <size_t BlockSize, size_t BlockCount>
inline StaticSlabPool<BlockSize, BlockCount>& sharedSlabPool() noexcept {
    static StaticSlabPool<BlockSize, BlockCount> instance;
    return instance;
}

// ── STL Allocator 适配器 ────────────────────────────────────────────────────
// 所有同 (BlockSize, BlockCount) 的实例共享一个 Meyers 单例 pool
// 用法：std::map<K, V, std::less<K>, PooledAllocator<std::pair<const K, V>, 512, 16>>
template <typename T, size_t BlockSize, size_t BlockCount>
class PooledAllocator {
public:
    using value_type = T;
    using pointer = T*;
    using const_pointer = const T*;
    using size_type = size_t;
    using difference_type = ptrdiff_t;

    // STL Allocator 必须支持的 propagation traits
    using propagate_on_container_copy_assignment = std::false_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::false_type;
    using is_always_equal = std::true_type;

    template <typename U>
    struct rebind {
        using other = PooledAllocator<U, BlockSize, BlockCount>;
    };

    PooledAllocator() noexcept = default;

    template <typename U>
    PooledAllocator(const PooledAllocator<U, BlockSize, BlockCount>&) noexcept {}

    T* allocate(size_t n) {
        const size_t bytes = n * sizeof(T);
        // 只对 n==1 且尺寸命中块大小的场景走池（std::map/std::set 节点都满足）
        if (n == 1 && bytes <= BlockSize) {
            void* p = pool().allocate();
            if (!p) throw std::bad_alloc();
            return static_cast<T*>(p);
        }
        // 大分配或批量分配（vector 等）回退 malloc
        void* p = ::malloc(bytes);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }

    void deallocate(T* p, size_t n) noexcept {
        if (!p) return;
        const size_t bytes = n * sizeof(T);
        if (n == 1 && bytes <= BlockSize) {
            pool().deallocate(p);
            return;
        }
        ::free(p);
    }

    // 获取池实例（跨 T 共享同 (BlockSize, BlockCount) 的全局单例池）
    static StaticSlabPool<BlockSize, BlockCount>& pool() {
        return sharedSlabPool<BlockSize, BlockCount>();
    }
};

// 相同池类型视为相等（is_always_equal = true_type）
template <typename T1, typename T2, size_t B, size_t N>
bool operator==(const PooledAllocator<T1, B, N>&, const PooledAllocator<T2, B, N>&) noexcept {
    return true;
}
template <typename T1, typename T2, size_t B, size_t N>
bool operator!=(const PooledAllocator<T1, B, N>& a, const PooledAllocator<T2, B, N>& b) noexcept {
    return !(a == b);
}

// ── 预设池参数（跨模块共享同一 Meyers 单例池）──────────────────────────
// SmallNodePool: 小节点 std::map/std::set 专用（高频增删的节点型容器）
//   - 64B/块 × 64 块 = 4KB DRAM 预分配
//   - 容纳 _Rb_tree_node<pair<String, ulong>>(~40B)、_Rb_tree_node<String>(~32B)
//             、_Rb_tree_node<pair<uint8_t, String>>(~36B)、_Rb_tree_node<pair<String, void*>>(~36B)
//   - 适用于：_failureBackoff / _runningRuleIds / _buttonEventCache /
//             actionTickers / pinToPeripheral / _pollSourceLast* 等高频小容器
static constexpr size_t SMALL_NODE_BLOCK_SIZE  = 64;
static constexpr size_t SMALL_NODE_BLOCK_COUNT = 64;

template <typename T>
using SmallNodeAllocator = PooledAllocator<T, SMALL_NODE_BLOCK_SIZE, SMALL_NODE_BLOCK_COUNT>;

} // namespace FastBee

#endif // FB_STATIC_POOL_ALLOCATOR_H
