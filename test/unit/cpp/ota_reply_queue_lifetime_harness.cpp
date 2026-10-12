#include <atomic>
#include <cassert>
#include <cstdlib>
#include <future>
#include <iostream>
#include <new>
#include <thread>
#include "protocols/OtaReplyQueueOwner.h"

static bool failState = false;
static unsigned stateAllocations = 0;
void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
    assert(criticalDepth == 0);
    ++stateAllocations;
    return failState ? nullptr : std::malloc(bytes);
}
void operator delete(void* value, const std::nothrow_t&) noexcept { std::free(value); }

using FastBee::OtaReplyQueueOwner;
int main() {
    assert(liveQueues.empty());
    {
        OtaReplyQueueOwner owner(136);
        assert(stateAllocations == 0 && createCalls == 0);
        assert(!owner.acquire());
        assert(owner.releaseIfIdle([] { return true; }));
        failState = true;
        assert(!owner.ensure() && liveQueues.empty());
        failState = false;
        for (unsigned queue = 1; queue <= 3; ++queue) {
            failQueueCall = createCalls + queue;
            assert(!owner.ensure());
            assert(liveQueues.empty()); // Every partial creation is rolled back.
        }
        failQueueCall = 0;
        {
            auto creationLease = owner.ensureAndAcquire();
            assert(creationLease && liveQueues.size() == 3);
            assert(!owner.releaseIfIdle([] { return true; })); // Publication retains command admission.
        }
        const auto calls = createCalls;
        assert(owner.ensure() && createCalls == calls);
        {
            auto lease = owner.acquire();
            assert(lease && lease.terminal()->capacity == 1 && lease.replies()->capacity == 8);
            assert(lease.progress()->capacity == 1 && lease.progress()->itemSize == 136);
            auto moved = std::move(lease);
            assert(!lease && moved);
            assert(!owner.releaseIfIdle([] { return true; })); // In-flight caller.
        }
        assert(!owner.releaseIfIdle([] { return false; })); // Active task or pending reply.
        for (unsigned slot = 0; slot < 3; ++slot) {
            QueueHandle_t queue;
            {
                auto lease = owner.acquire();
                queue = slot == 0 ? lease.terminal() : slot == 1 ? lease.replies() : lease.progress();
                queue->count = 1;
            }
            assert(!owner.releaseIfIdle([] { return true; })); // Queued terminal/reply/progress.
            { auto lease = owner.acquire(); assert(lease); queue->count = 0; }
        }
        deleteHook = [&] { assert(!owner.acquire()); assert(!owner.ensure()); };
        assert(owner.releaseIfIdle([] { return true; }));
        assert(liveQueues.empty());
        for (unsigned cycle = 0; cycle < 64; ++cycle) {
            assert(owner.ensure());
            { auto lease = owner.acquire(); assert(lease); }
            assert(owner.releaseIfIdle([] { return true; }));
            assert(liveQueues.empty());
        }
        // Hold actual allocation in a worker while another caller attempts admission/retirement.
        std::promise<void> entered, continueCreate;
        auto continuation = continueCreate.get_future();
        createHook = [&] { entered.set_value(); continuation.wait(); };
        auto creating = std::async(std::launch::async, [&] { return owner.ensure(); });
        entered.get_future().wait();
        assert(!owner.acquire() && !owner.ensure());
        assert(!owner.releaseIfIdle([] { return true; }));
        continueCreate.set_value();
        assert(creating.get());
        std::atomic<bool> stop{false};
        std::thread reader([&] {
            while (!stop) {
                auto lease = owner.acquire();
                if (lease) {
                    assert(lease.terminal()->capacity == 1 && lease.replies()->capacity == 8);
                    std::this_thread::yield();
                    assert(lease.progress()->itemSize == 136);
                }
            }
        });
        for (unsigned cycle = 0; cycle < 2000; ++cycle) {
            owner.releaseIfIdle([] { return true; });
            owner.ensure();
        }
        stop = true;
        reader.join();
        assert(owner.releaseIfIdle([] { return true; }));
    }
    // Lease owns State directly: the handles survive destruction of their original owner.
    {
        auto detachedLease = [] {
            OtaReplyQueueOwner owner(136);
            assert(owner.ensure());
            return owner.acquire();
        }();
        assert(detachedLease && liveQueues.size() == 3);
        assert(detachedLease.replies()->capacity == 8);
    }
    assert(liveQueues.empty());
    assert(createCalls - 3 == deleteCalls); // Three injected queue allocation failures.
    std::cout << "OTA reply queue lifecycle checks passed\n";
}
