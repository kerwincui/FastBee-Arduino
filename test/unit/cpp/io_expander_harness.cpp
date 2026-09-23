#include "peripherals/IoExpander.h"
#include <cassert>
#include <cstdint>
#include <vector>

struct FakeTransport final : FastBee::IoExpanderTransport {
    std::vector<uint8_t> writes;
    uint8_t sample = 0xff;
    bool writeOk = true, readOk = true;
    bool writeBusy = false, readBusy = false;
    unsigned reads = 0;
    FastBee::IoExpanderResult writeByte(uint8_t value) override {
        if (writeBusy) return FastBee::IoExpanderResult::Busy;
        writes.push_back(value);
        return writeOk ? FastBee::IoExpanderResult::Ok : FastBee::IoExpanderResult::Error;
    }
    FastBee::IoExpanderResult readByte(uint8_t& value) override {
        if (readBusy) return FastBee::IoExpanderResult::Busy;
        ++reads; value = sample;
        return readOk ? FastBee::IoExpanderResult::Ok : FastBee::IoExpanderResult::Error;
    }
};

static void shutdown_is_one_attempt_and_never_claims_unconfirmed_safe_levels() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    assert(driver.begin(3, 0, 20, 0));
    assert(driver.writeChannel(0, true, 0));
    const size_t before = transport.writes.size();
    assert(driver.shutdown(0xfc) == FastBee::IoExpanderResult::Ok);
    assert(transport.writes.size() == before + 1 && transport.writes.back() == 0xfc);
    assert(driver.outputShadow() == 0xfc && !driver.healthy(0));
    assert(!driver.poll(100) && !driver.writeChannel(0, true, 100));

    assert(driver.begin(3, 0, 20, 200));
    assert(driver.writeChannel(0, true, 200));
    transport.writeBusy = true;
    const size_t busyWrites = transport.writes.size();
    assert(driver.shutdown(0xfc) == FastBee::IoExpanderResult::Busy);
    assert(transport.writes.size() == busyWrites && driver.outputShadow() == 0xfd);
    assert(!driver.healthy(200));
    transport.writeBusy = false;
    transport.writeOk = false;
    assert(driver.shutdown(0xfc) == FastBee::IoExpanderResult::Error);
    assert(driver.outputShadow() == 0xfd && !driver.healthy(200));
    transport.writeOk = true;
    assert(driver.shutdown(0xfc) == FastBee::IoExpanderResult::Ok);
    assert(driver.outputShadow() == 0xfc && !driver.healthy(200));
}

static void shared_port_and_channel_lifecycle() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    transport.sample = 0xf8; // input P2 externally pulled low; outputs P0/P1 low
    assert(driver.begin(0x03, 0x00, 20, 100, 0x04));
    assert(transport.writes.back() == 0xfc); // inputs and unused pins released
    bool high = true;
    assert(driver.readChannel(2, false, 100, high) && !high);
    assert(!driver.writeChannel(2, false, 100));
    assert(driver.writeChannel(0, true, 100));
    assert(transport.writes.back() == 0xfd); // never write sampled P2 LOW back
    assert(driver.writeChannel(1, true, 100));
    assert(transport.writes.back() == 0xff); // serialized writes preserve sibling
    assert(driver.readChannel(1, true, 100, high) && high); // command vs sampled pin
    const size_t beforeRename = transport.writes.size();
    assert(driver.configure(0x03, 0x00, 101, 0x04));
    assert(transport.writes.size() == beforeRename);
    assert(driver.configure(0x07, 0x00, 102));
    assert(transport.writes.back() == 0xfb); // new P2 LOW, P0/P1 stay HIGH
    assert(driver.configure(0x06, 0x00, 103));
    assert(transport.writes.back() == 0xfa); // retiring P0 restores its startup LOW
    assert(driver.writeChannel(1, false, 103));
    assert(transport.writes.back() == 0xf8); // later sibling writes preserve retired P0
    assert(driver.configure(0x06, 0x00, 104, 0x01));
    assert(transport.writes.back() == 0xf9); // reuse P0 as input releases it HIGH
    assert(!driver.configure(0x06, 0x00, 104, 0x02)); // incompatible direction claim
    assert(!driver.readChannel(8, false, 104, high));
    assert(!driver.readChannel(1, false, 104, high));
}

static void read_failure_and_recovery() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    assert(driver.begin(0x03, 0, 20, 0));
    assert(driver.writeChannel(0, true, 0));
    const uint32_t generation = driver.generation();
    transport.readOk = false;
    assert(!driver.poll(20));
    bool high = true;
    assert(!driver.readChannel(0, true, 20, high));
    assert(!driver.readChannel(2, false, 20, high));
    assert(!driver.writeChannel(1, true, 20));
    transport.readOk = true;
    assert(driver.poll(40));
    assert(transport.writes.size() == 2); // one transient NACK does not reset outputs
    assert(driver.outputShadow() == 0xfd && driver.generation() != generation);
    transport.readOk = false;
    assert(!driver.poll(60));
    assert(!driver.poll(80));
    assert(!driver.poll(100));
    assert(driver.failures() == 3);
    transport.readOk = true;
    assert(!driver.poll(349));
    assert(driver.poll(350));
    assert(transport.writes.back() == 0xfc); // confirmed outage restores startup
    assert(driver.outputShadow() == 0xfc && driver.failures() == 0);
}

static void failed_write_invalidates_entire_port() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    assert(driver.begin(0x03, 0, 20, 0));
    transport.writeOk = false;
    assert(!driver.writeChannel(0, true, 1));
    bool high;
    assert(!driver.readChannel(1, true, 1, high));
    const size_t writes = transport.writes.size();
    assert(!driver.writeChannel(1, true, 1));
    assert(transport.writes.size() == writes);
    transport.writeOk = true;
    assert(!driver.poll(250));
    assert(driver.poll(251));
    assert(transport.writes.back() == 0xfc);
}

static void freshness_wraparound_and_initial_absence() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    assert(driver.begin(1, 0, 20, UINT32_MAX - 10));
    assert(driver.writeChannel(0, true, UINT32_MAX - 9));
    assert(driver.poll(9)); // 20ms across uint32 wrap
    assert(transport.reads == 2);
    assert(driver.healthy(69));
    assert(!driver.healthy(70));
    bool high;
    assert(!driver.readChannel(0, true, 70, high));
    const uint32_t generation = driver.generation();
    const size_t writes = transport.writes.size();
    assert(driver.poll(70));
    assert(transport.writes.size() == writes); // scheduler latency must not replay startup outputs
    assert(driver.outputShadow() == 0xff);
    assert(driver.generation() != generation);
    driver.end();
    assert(!driver.healthy(70));
    transport.writeOk = false;
    assert(!driver.begin(1, 0, 20, 100));
    transport.writeOk = true;
    assert(!driver.poll(349));
    assert(driver.poll(350)); // persisted absent devices can recover without reboot
}

static void busy_does_not_invalidate_outputs_or_increment_failures() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    assert(driver.begin(1, 0, 20, 0));
    assert(driver.writeChannel(0, true, 0));
    const uint32_t generation = driver.generation();
    transport.writeBusy = true;
    assert(!driver.writeChannel(0, false, 1));
    assert(driver.healthy(1) && driver.outputShadow() == 0xff && driver.failures() == 0);
    assert(!driver.configure(3, 0, 2));
    assert(!driver.writeChannel(1, true, 3)); // busy configure did not commit its mask
    transport.writeBusy = false;
    assert(driver.configure(3, 0, 4));
    assert(driver.outputShadow() == 0xfd); // retry still applies new P1 startup LOW
    transport.readBusy = true;
    const size_t writes = transport.writes.size();
    assert(driver.poll(20));
    assert(driver.poll(40));
    assert(driver.poll(60));
    assert(!driver.poll(61)); // data ages naturally; contention is not a NACK
    assert(driver.failures() == 0 && driver.generation() == generation);
    transport.readBusy = false;
    assert(driver.poll(80));
    assert(driver.failures() == 0 && driver.generation() != generation);
    assert(driver.outputShadow() == 0xfd && transport.writes.size() == writes);
}

static void outage_backoff_is_bounded_and_resets_on_success() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    assert(driver.begin(1, 0, 20, 0));
    transport.readOk = false;
    assert(!driver.poll(20));
    assert(!driver.poll(40));
    assert(!driver.poll(60)); // third failure starts 250ms recovery cooldown
    transport.writeOk = false;
    uint32_t due = 60;
    const uint16_t delays[] = {250, 500, 1000, 2000, 2000};
    for (uint16_t delay : delays) {
        const size_t writes = transport.writes.size();
        assert(!driver.poll(due + delay - 1));
        assert(transport.writes.size() == writes);
        due += delay;
        assert(!driver.poll(due));
        assert(transport.writes.size() == writes + 1);
    }
    const uint8_t failures = driver.failures();
    transport.writeBusy = true;
    due += 2000;
    assert(!driver.poll(due));
    assert(driver.failures() == failures); // busy does not extend the backoff
    transport.writeBusy = false;
    transport.writeOk = transport.readOk = true;
    assert(driver.poll(due + 1));
    assert(driver.failures() == 0 && driver.outputShadow() == 0xfe);
    const unsigned reads = transport.reads;
    assert(driver.poll(due + 20));
    assert(transport.reads == reads); // only 19ms since the successful sample
    assert(driver.poll(due + 21));
    assert(transport.reads == reads + 1); // configured cadence restored
    transport.writeOk = false;
    assert(!driver.writeChannel(0, true, due + 22));
    const size_t writes = transport.writes.size();
    assert(!driver.poll(due + 271));
    assert(transport.writes.size() == writes);
    assert(!driver.poll(due + 272)); // new outage starts at 250ms again
    assert(transport.writes.size() == writes + 1);
}

static void recovery_read_busy_does_not_repeat_acknowledged_startup_write() {
    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    transport.readBusy = true;
    assert(!driver.begin(1, 0, 20, 0));
    assert(driver.failures() == 0 && transport.writes.size() == 1);
    assert(!driver.poll(20));
    assert(transport.writes.size() == 1);
    transport.readBusy = false;
    assert(driver.poll(21));
    assert(driver.failures() == 0 && transport.writes.size() == 1);
}

static void retry_cadence_respects_slow_sampling_and_millis_wrap() {
    FakeTransport slowTransport;
    FastBee::Pcf8574 slow(slowTransport);
    assert(slow.begin(1, 0, 1000, 0));
    slowTransport.writeOk = false;
    assert(!slow.writeChannel(0, true, 1));
    const size_t slowWrites = slowTransport.writes.size();
    assert(!slow.poll(251));
    assert(!slow.poll(1000));
    assert(slowTransport.writes.size() == slowWrites);
    assert(!slow.poll(1001));
    assert(slowTransport.writes.size() == slowWrites + 1);

    FakeTransport transport;
    FastBee::Pcf8574 driver(transport);
    const uint32_t start = UINT32_MAX - 100;
    assert(driver.begin(1, 0, 20, start));
    transport.writeOk = false;
    assert(!driver.writeChannel(0, true, start + 1));
    const size_t writes = transport.writes.size();
    assert(!driver.poll(149));
    assert(transport.writes.size() == writes);
    transport.writeOk = true;
    assert(driver.poll(150)); // 250ms retry deadline across uint32 wrap
    assert(transport.writes.size() == writes + 1 && driver.failures() == 0);
}

int main() {
    shutdown_is_one_attempt_and_never_claims_unconfirmed_safe_levels();
    shared_port_and_channel_lifecycle();
    read_failure_and_recovery();
    failed_write_invalidates_entire_port();
    freshness_wraparound_and_initial_absence();
    busy_does_not_invalidate_outputs_or_increment_failures();
    outage_backoff_is_bounded_and_resets_on_success();
    recovery_read_busy_does_not_repeat_acknowledged_startup_write();
    retry_cadence_respects_slow_sampling_and_millis_wrap();
}
