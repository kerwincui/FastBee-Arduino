#include <Arduino.h>
static uint32_t pcaClock = 0;
static uint32_t providerMicros() { return pcaClock; }
#define micros providerMicros
#include "peripherals/PCA9685Provider.h"
#undef micros
#include <cstdio>
#include <cstdlib>
#include <future>
#include <limits>
#include <string>
#include <thread>

using namespace FastBee;
using P = PCA9685Provider;
using S = PCA9685OutputSession;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "line %d: %s\n", __LINE__, #c); std::exit(86); } } while(0)
struct Trace {
    bool owns = true, gated = true, disableOk = true, enableOk = true, releaseOk = true;
    int pin = 4;
    unsigned disables = 0, enables = 0, releases = 0, destroyed = 0;
};
struct Gate final : PCA9685OutputLease {
    Trace& trace;
    explicit Gate(Trace& value) : trace(value) {}
    ~Gate() override { ++trace.destroyed; }
    int pin() const override { return trace.pin; }
    bool owned() const override { return trace.owns; }
    bool disabled() const override { return trace.owns && trace.gated; }
    bool disable() override { ++trace.disables; if (trace.disableOk && trace.owns) trace.gated = true; return trace.disableOk && trace.owns; }
    bool enable() override { ++trace.enables; if (trace.enableOk && trace.owns) trace.gated = false; return trace.enableOk && trace.owns; }
    bool release() override {
        ++trace.releases;
        CHECK(I2CBusManager::instance().clientCount() == 0);
        if (!trace.releaseOk) return false;
        trace.owns = false; return true;
    }
};
static std::unique_ptr<P> make(Trace& trace) {
    return std::make_unique<P>(21, 22, 100000, 0x40, std::make_unique<Gate>(trace));
}
static bool begin(P& p) { return p.begin(S::ResetEvidence::ConfirmedPowerOn, 50, 25000000, 3, false); }
static void ready(P& p) {
    CHECK(begin(p) && p.state() == P::State::Waking);
    pcaClock += 499; CHECK(p.service() && !p.ready());
    ++pcaClock; CHECK(p.service() && p.state() == P::State::ReadyDisabled);
}
static void admission() {
    Trace t; auto p = make(t);
    CHECK(!p->begin(S::ResetEvidence::Unknown, 50, 25000000, 3, false));
    CHECK(!p->begin(S::ResetEvidence::ConfirmedPowerOn, 50, 25000000, 0x10000, false));
    CHECK(Wire.transmissions == 0 && t.disables == 0 && t.enables == 0);
    ready(*p); const auto count = Wire.transmissions;
    CHECK(!begin(*p) && p->error() == P::Error::AlreadyAttempted);
    CHECK(p->ready() && Wire.transmissions == count && t.enables == 0);
    CHECK(p->shutdown() == P::StopResult::Ok);
}
static void normal() {
    Trace t; auto p = make(t); ready(*p); CHECK(t.gated);
    CHECK(I2CBusManager::instance().clientCount() == 2);
    CHECK(p->setTicks(1, 4000, 200) && t.gated);
    PCA9685ChannelRegisters value;
    const auto count = Wire.transmissions;
    for (int n = 0; n < 1000; ++n) CHECK(p->knownChannel(1, value));
    CHECK(Wire.transmissions == count && value.bytes[0] == 0xa0 && value.bytes[2] == 0x68);
    CHECK(!p->knownChannel(2, value) && !p->setTicks(2, 0, 100));
    CHECK(!p->setTicks(0, 4096, 1) && !p->setTicks(0, 0, 4097));
    CHECK(Wire.transmissions == count);
    CHECK(p->enableOutputs() && !t.gated && t.enables == 1);
    CHECK(p->enableOutputs() && !t.gated && t.enables == 1);
    CHECK(p->disableOutputs() && t.gated && p->ready());
    CHECK(p->enableOutputs() && t.enables == 2);
    CHECK(p->shutdown() == P::StopResult::Ok && t.gated && !t.owns);
    const auto disables = t.disables;
    CHECK(!p->enableOutputs() && !p->setTicks(0, 0, 10) && !p->knownChannel(0, value));
    CHECK(p->shutdown() == P::StopResult::Ok && t.releases == 1);
    p.reset(); CHECK(t.disables == disables && t.destroyed == 1);
}
static void conversions() {
    Trace t; auto p = make(t); ready(*p);
    CHECK(p->calculatedFrequencyHz() > 50 && p->calculatedFrequencyHz() < 50.1);
    CHECK(p->setPulseMicros(0, 1500) && Wire.registers[8] == 0x33 && Wire.registers[9] == 1);
    CHECK(p->setServoAngle(0, 90, 0, 180, 500, 2500) && Wire.registers[8] == 0x33);
    CHECK(p->setDutyPercent(0, 100) && Wire.registers[7] == 0x10 && Wire.registers[9] == 0);
    CHECK(p->setDutyPercent(0, 0) && Wire.registers[7] == 0 && Wire.registers[9] == 0x10);
    CHECK(p->setDutyPercent(0, 50) && Wire.registers[9] == 8);
    CHECK(p->setDutyPercent(0, 0.00001) && Wire.registers[8] == 1 && Wire.registers[9] == 0);
    CHECK(p->setDutyPercent(0, 99.99999) && Wire.registers[7] == 0 && Wire.registers[9] == 15);
    const auto count = Wire.transmissions;
    CHECK(!p->setPulseMicros(0, 0) && !p->setPulseMicros(0, 20000));
    CHECK(!p->setServoAngle(0, 181, 0, 180, 500, 2500));
    CHECK(!p->setServoAngle(0, 90, 0, 180, 2500, 500));
    CHECK(!p->setServoAngle(0, 0, -1e308, 1e308, 500, 2500));
    CHECK(!p->setServoAngle(0, 90, 0, 180, 1500, 1500.01));
    CHECK(!p->setDutyPercent(0, std::numeric_limits<double>::quiet_NaN()));
    CHECK(!p->setDutyPercent(0, std::numeric_limits<double>::infinity()));
    CHECK(!p->setDutyPercent(0, -1) && !p->setDutyPercent(0, 101));
    CHECK(Wire.transmissions == count && t.gated && p->failures() == 0);
    CHECK(p->shutdown() == P::StopResult::Ok);
}
static void lost(bool pin) {
    Trace t; auto p = make(t); ready(*p); CHECK(p->enableOutputs());
    if (pin) t.pin = 5; else t.owns = false;
    const auto count = Wire.transmissions, disables = t.disables;
    CHECK(!p->setTicks(0, 0, 100) && p->state() == P::State::Faulted);
    CHECK(Wire.transmissions == count && t.disables == disables);
    CHECK(p->shutdown() == P::StopResult::Pending && I2CBusManager::instance().clientCount() == 2);
    CHECK(t.disables == disables);
    t.pin = 4; t.owns = true;
    CHECK(p->shutdown() == P::StopResult::Error && t.gated && t.releases == 1);
    CHECK(p->firstFailure() == P::Error::GateOwnership);
}
static void fault(const std::string& mode) {
    Trace t; auto p = make(t); ready(*p); CHECK(p->enableOutputs());
    if (mode == "drift") Wire.registers[0xfe] ^= 1;
    else if (mode == "applied_nack") { Wire.nackDataRegister = 6; Wire.applyFailedWrite = true; }
    else Wire.ack = false;
    CHECK(!p->setTicks(0, 0, 100) && t.gated && !p->ready());
    if (mode == "applied_nack") CHECK(Wire.registers[8] == 100 && Wire.registers[9] == 0);
    PCA9685ChannelRegisters value; CHECK(!p->knownChannel(0, value));
    const auto count = Wire.transmissions;
    CHECK(!p->service() && !p->enableOutputs() && !begin(*p));
    CHECK(Wire.transmissions == count);
    CHECK(p->shutdown() == P::StopResult::Error && p->failures() > 0);
}
static void beginFailure(bool collision) {
    Trace t; auto p = make(t); auto& manager = I2CBusManager::instance();
    String detail; int other;
    if (collision) {
        CHECK(manager.acquire(&other, 21, 22, 100000, detail));
        CHECK(manager.claimAddress(&other, 0x70, detail));
    } else Wire.shortRead = true;
    CHECK(!begin(*p) && p->state() == P::State::Faulted && t.gated);
    CHECK(p->firstFailure() == P::Error::Begin);
    const auto count = Wire.transmissions;
    Wire.shortRead = false;
    if (collision) CHECK(manager.release(&other));
    CHECK(!begin(*p) && p->error() == P::Error::AlreadyAttempted);
    CHECK(Wire.transmissions == count && t.enables == 0);
    CHECK(p->shutdown() == P::StopResult::Error && t.releases == 1);
}
static void enableFailure() {
    Trace t; auto p = make(t); ready(*p); t.enableOk = false;
    CHECK(!p->enableOutputs() && !p->ready() && t.gated);
    CHECK(p->firstFailure() == P::Error::Gate);
    t.enableOk = true; const auto count = Wire.transmissions;
    CHECK(!p->enableOutputs() && Wire.transmissions == count && t.enables == 1);
    CHECK(p->shutdown() == P::StopResult::Error);
}
static void changedDestructor() {
    Trace t; auto p = make(t); ready(*p); CHECK(p->enableOutputs()); t.pin = 5;
    const auto count = Wire.transmissions, disables = t.disables;
    p.reset();
    CHECK(Wire.transmissions == count && t.disables == disables && t.destroyed == 1);
    CHECK(I2CBusManager::instance().clientCount() == 0);
    // A fake lease destructor does not confirm release/gating of a physical pad.
    CHECK(t.releases == 0 && !t.gated);
}
static void busy(bool atBegin) {
    Trace t; auto p = make(t); if (!atBegin) { ready(*p); CHECK(p->enableOutputs()); }
    auto& manager = I2CBusManager::instance();
    std::promise<void> locked, release; auto signal = release.get_future();
    std::thread other([&] { CHECK(manager.lock()); locked.set_value(); signal.wait(); manager.unlock(); });
    locked.get_future().wait(); const auto count = Wire.transmissions;
    if (atBegin) CHECK(!begin(*p) && p->state() == P::State::Idle);
    else {
        CHECK(!p->setTicks(0, 0, 100) && p->state() == P::State::Enabled);
        CHECK(p->shutdown() == P::StopResult::Pending && t.gated);
    }
    CHECK(p->error() == P::Error::Busy && Wire.transmissions == count && t.releases == 0);
    release.set_value(); other.join();
    if (atBegin) ready(*p);
    CHECK(p->shutdown() == P::StopResult::Ok);
}
static void releaseRetry() {
    Trace t; auto p = make(t); ready(*p); t.releaseOk = false;
    CHECK(p->shutdown() == P::StopResult::Pending && t.gated && t.owns && t.releases == 1);
    const auto count = Wire.transmissions, disables = t.disables;
    CHECK(p->shutdown() == P::StopResult::Pending && t.disables == disables && Wire.transmissions == count);
    t.releaseOk = true; CHECK(p->shutdown() == P::StopResult::Ok && t.releases == 3);
}
static void stopFailure() {
    Trace t; auto p = make(t); ready(*p); CHECK(p->enableOutputs()); t.disableOk = false;
    CHECK(p->shutdown() == P::StopResult::Pending && !t.gated && t.releases == 0);
    CHECK(I2CBusManager::instance().clientCount() == 2);
    t.disableOk = true; CHECK(p->shutdown() == P::StopResult::Error && t.gated && !t.owns);
    CHECK(p->shutdown() == P::StopResult::Error && p->firstFailure() == P::Error::Gate);
}
static void wakeTimeout() {
    Trace t; auto p = make(t); CHECK(begin(*p)); pcaClock += 1000000;
    CHECK(!p->service() && p->sessionFailure() == S::Error::WakeTimeout && t.gated);
    CHECK(p->shutdown() == P::StopResult::Error);
}
static void unexpectedGate() {
    Trace t; auto p = make(t); ready(*p); t.gated = false;
    const auto count = Wire.transmissions;
    CHECK(!p->setTicks(0, 0, 100) && t.gated && Wire.transmissions == count);
    CHECK(p->shutdown() == P::StopResult::Error);
}
int main(int argc, char** argv) {
    CHECK(argc == 2); const std::string mode = argv[1];
    if (mode == "admission") admission(); else if (mode == "normal") normal();
    else if (mode == "conversions") conversions(); else if (mode == "gate_lost") lost(false);
    else if (mode == "pin_changed") lost(true); else if (mode == "busy") busy(false);
    else if (mode == "begin_busy") busy(true); else if (mode == "release_retry") releaseRetry();
    else if (mode == "stop_failure") stopFailure(); else if (mode == "wake_timeout") wakeTimeout();
    else if (mode == "unexpected_gate") unexpectedGate();
    else if (mode == "wrap") { pcaClock = UINT32_MAX - 400; normal(); }
    else if (mode == "drift" || mode == "transport" || mode == "applied_nack") fault(mode);
    else if (mode == "begin_failure") beginFailure(false);
    else if (mode == "alias_collision") beginFailure(true);
    else if (mode == "enable_failure") enableFailure();
    else if (mode == "changed_destructor") changedDestructor(); else CHECK(false);
    std::printf("%s PASS\n", mode.c_str());
}
