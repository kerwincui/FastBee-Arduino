#include "core/InternalAtomicWord.h"
#include "core/ReportInstanceBinding.h"
#include "protocols/MqttClientLease.h"
#include <cassert>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using Word = FastBee::InternalAtomicWord;
using Gate = FastBee::MqttClientLifetime;
static_assert(!std::is_copy_constructible<Word>::value && !std::is_move_constructible<Word>::value);
static_assert(!std::is_copy_assignable<Word>::value && !std::is_move_assignable<Word>::value);
static_assert(!std::is_copy_constructible<Gate>::value && !std::is_move_constructible<Gate>::value);
static_assert(!std::is_copy_assignable<Gate>::value && !std::is_move_assignable<Gate>::value);

int main(int argc, char** argv) {
    assert(argc == 2);
    const std::string mode = argv[1];
    using namespace AtomicSdkMock;
    allInternal = mode == "inline";
    if (mode == "oom" || mode == "allocator_region") {
        failAllocation = mode == "oom";
        wrongRegion = mode == "allocator_region";
        {
            Gate gate;
            FastBee::ReportInstanceSequence sequence(80);
            assert(!gate.available() && allocations == 2);
            failAllocation = false;
            wrongRegion = false;
            for (unsigned i = 0; i < 1000; ++i) {
                Gate::Read read(gate);
                Gate::Write write(gate);
                assert(!read && !write && !write.tryAcquire());
                read.release(); write.release();
                assert(sequence.next() == 0 && !gate.available());
            }
            assert(allocations == 2); // No hidden allocation retry on the hot path.
        }
        assert(internalAllocations.empty() && frees == (mode == "oom" ? 0U : 2U));
        {
            Gate fresh;
            assert(fresh.available() && Gate::Read(fresh));
        }
        assert(allocations == 3 && internalAllocations.empty());
        return 0;
    }
    {
        Gate gate;
        assert(gate.available());
        const auto before = allocations;
        if (mode == "threads") {
            std::atomic<unsigned> admitted{0};
            std::vector<std::thread> workers;
            for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
                for (unsigned n = 0; n < 2000; ++n) {
                    Gate::Read read(gate);
                    if (read) admitted.fetch_add(1);
                }
            });
            for (auto& worker : workers) worker.join();
            assert(admitted.load() > 0);
            assert(Gate::Write(gate)); // All admitted readers released, without loss.
        } else {
            for (unsigned i = 0; i < 1000; ++i) {
                Gate::Read first(gate), second(gate);
                assert(first && second && !Gate::Write(gate));
                Gate::Read moved(std::move(first));
                assert(moved && !first);
                second.release(); moved.release();
                Gate::Write write(gate);
                assert(write && !Gate::Read(gate));
                write.release();
            }
        }
        assert(allocations == before);
        FastBee::ReportInstanceSequence sequence(UINT32_MAX - 1);
        assert(sequence.next() == UINT32_MAX && sequence.next() == 0 && sequence.next() == 0);
        Word word(73);
        assert(word.get() && word.get()->load() == 73);
        uint32_t expected = 73;
        assert(word.get()->compare_exchange_strong(expected, 74));
        assert(word.get()->fetch_sub(1) == 74 && word.get()->load() == 73);
    }
    assert(internalAllocations.empty());
    assert(allocations == (allInternal ? 0U : 3U) && frees == allocations);
}
