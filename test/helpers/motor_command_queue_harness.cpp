#include "core/MotorCommandQueue.h"
#include <cassert>
#include <cstdio>
using namespace MotorCommands;
static int submit(Queue& q, const char* ref, const char* action, const char* request,
                  uint32_t now = 10, int value = 0) {
    int slot = -1; uint32_t existing = 0; const char* error = nullptr;
    if (!q.reserve(ref, action, value, request, now, slot, existing, error)) return -1;
    if (existing) return -2;
    if (!std::strcmp(action, "stop")) q.cancelQueued(ref, now);
    auto& c = q.commands[slot]; c = Command{};
    std::strcpy(c.ref, ref); std::strcpy(c.action, action); std::strcpy(c.requestId, request);
    c.value = value; c.id = q.allocateId(); c.order = ++q.nextOrder; c.phase = Phase::QUEUED;
    return slot;
}
int main() {
    Queue q;
    int launch = submit(q, "motor.1.a", "forward", "click-1");
    assert(launch >= 0);
    assert(submit(q, "motor.1.a", "forward", "click-1") == -2);
    assert(submit(q, "motor.1.a", "reverse", "click-1") == -1);
    assert(submit(q, "motor.1.a", "forward", "click-2") == -1);
    int stop = submit(q, "motor.1.a", "stop", "stop-1");
    assert(stop >= 16 && q.commands[launch].phase == Phase::DONE);
    assert(!q.commands[launch].success && std::strlen(q.commands[launch].error));
    assert(q.selectNext() == stop);
    assert(submit(q, "motor.1.a", "forward", "click-1") == -2);
    // A replay stored after a running entry must still return its own result.
    q.commands[stop].phase = Phase::DONE; q.commands[stop].finishedMs = 10;
    q.commands[launch].phase = Phase::RUNNING;
    assert(submit(q, "motor.1.a", "stop", "stop-1") == -2);

    Queue full;
    for (int n = 0; n < 16; ++n) {
        char request[20]; std::snprintf(request, sizeof(request), "req-%d", n);
        int s = submit(full, "motor.1.a", "forward", request);
        assert(s >= 0); full.commands[s].phase = Phase::DONE; full.commands[s].finishedMs = 10;
    }
    assert(submit(full, "motor.1.a", "forward", "overflow") == -1);
    assert(submit(full, "motor.1.a", "stop", "urgent") >= 16);
    assert(submit(full, "motor.2.b", "forward", "expired", 30010) >= 0);

    Queue fifo;
    int a = submit(fifo, "motor.a", "forward", "a");
    int b = submit(fifo, "motor.b", "forward", "b");
    assert(fifo.selectNext() == a);
    fifo.commands[a].phase = Phase::RUNNING;
    int stopA = submit(fifo, "motor.a", "stop", "stopA");
    int stopB = submit(fifo, "motor.b", "stop", "stopB");
    assert(fifo.commands[a].phase == Phase::RUNNING);
    assert(fifo.commands[b].phase == Phase::DONE);
    assert(fifo.selectNext() == stopA);
    fifo.commands[stopA].phase = Phase::DONE;
    assert(fifo.selectNext() == stopB);

    Queue wrap;
    wrap.nextOrder = UINT32_MAX - 1;
    int w1 = submit(wrap, "one", "forward", "one");
    int w2 = submit(wrap, "two", "forward", "two");
    assert(wrap.selectNext() == w1);
    wrap.nextId = 0x7ffffffeU;
    assert(wrap.allocateId() != wrap.commands[w1].id);
    assert(wrap.allocateId() != wrap.commands[w2].id);
    wrap.commands[w1].phase = Phase::DONE; wrap.commands[w1].finishedMs = UINT32_MAX - 15;
    int slot; uint32_t existing; const char* error = nullptr;
    assert(wrap.reserve("one", "forward", 0, "one", 14, slot, existing, error) && existing);
    assert(wrap.reserve("one", "forward", 0, "one", 30000, slot, existing, error) && !existing);
    std::puts("Motor queue passed: replay, conflict, stop reserve/cancel/priority, FIFO, expiry and wrap.");
}
