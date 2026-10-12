#include "peripherals/drivers/ADS1115ConversionSession.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <initializer_list>
#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "CHECK failed line %d: %s\n", __LINE__, #condition); std::exit(86); } } while (false)
using Session = FastBee::ADS1115ConversionSession;
using State = Session::State;
using Operation = Session::Operation;
using Error = Session::Error;

uint16_t start(Session& session, uint64_t token, uint32_t now = 0) {
    CHECK(session.operation() == Operation::ReadStatus);
    CHECK(session.observeStatus(token, 0x8583, now));
    CHECK(session.operation() == Operation::WriteStart);
    const uint16_t config = session.writeValue();
    CHECK(session.acknowledgeWrite(token, true, now));
    return config;
}

int main(int argc, char** argv) {
    CHECK(argc == 2); const std::string mode = argv[1];
    Session session;
    CHECK(session.state() == State::Idle && session.operation() == Operation::None);
    if (mode == "all_configs") {
        for (unsigned mux = 0; mux < 8; ++mux)
        for (unsigned gain : {6144U,4096U,2048U,1024U,512U,256U})
        for (unsigned rate : {8U,16U,32U,64U,128U,250U,475U,860U}) {
            auto token = session.begin(mux, gain, rate, 0, 500); CHECK(token);
            const auto config = start(session, token);
            CHECK(session.observeStatus(token, config & 0x7fff, 1));
            CHECK(session.operation() == Operation::ReadStatus);
            CHECK(session.observeStatus(token, config, 2));
            CHECK(session.operation() == Operation::ReadSample);
            CHECK(session.observeSample(token, 0xffff, 3));
            CHECK(session.state() == State::Complete && session.sample().valid);
            CHECK(session.sample().signedCode == -1 && session.sample().volts < 0);
            CHECK(session.sampleTimeMs() == 3 && session.error() == Error::None);
        }
    } else if (mode == "drain_single" || mode == "drain_continuous") {
        const auto token = session.begin(4,2048,128,0,500);
        if (mode == "drain_continuous") {
            CHECK(session.observeStatus(token, 0x42e3, 1));
            CHECK(session.operation() == Operation::WriteIdle && session.writeValue() == 0x43e3);
            CHECK(session.acknowledgeWrite(token, true, 2));
        }
        CHECK(session.observeStatus(token, 0x43e3, 3));
        CHECK(session.operation() == Operation::ReadStatus);
        CHECK(!session.observeSample(token, 0x1234, 4) && !session.sample().valid);
        CHECK(session.observeStatus(token, 0xc3e3, 5));
        CHECK(session.operation() == Operation::WriteStart);
        CHECK(session.writeValue() != 0xc3e3);
    } else if (mode == "stale_idle") {
        const auto token = session.begin(4,2048,128,0,50);
        const auto config = start(session, token);
        for (unsigned time = 1; time < 50; ++time) {
            CHECK(session.observeStatus(token, config, time));
            CHECK(session.operation() == Operation::ReadStatus && !session.sample().valid);
        }
        CHECK(!session.tick(token, 50));
        CHECK(session.error() == Error::StartNotObserved && !session.sample().valid);
    } else if (mode == "config_loss") {
        for (unsigned bit = 0; bit < 15; ++bit) {
            const auto token = session.begin(4,2048,128,0,500);
            const auto config = start(session, token);
            CHECK(!session.observeStatus(token, config ^ (1U << bit), 1));
            CHECK(session.error() == Error::ConfigLost && !session.sample().valid);
        }
    } else if (mode == "cancel_generation") {
        const auto old = session.begin(4,2048,128,0,500);
        const auto config = start(session, old);
        CHECK(session.observeStatus(old, config & 0x7fff, 1));
        CHECK(session.cancel(old) && session.state() == State::Cancelled);
        CHECK(!session.observeStatus(old, config, 2) && !session.observeSample(old, 1, 2));
        const auto current = session.begin(0,256,8,10,500); CHECK(current != old);
        CHECK(!session.observeStatus(old, config, 1000));
        CHECK(!session.acknowledgeWrite(old, true, 1000));
        CHECK(!session.transportFailed(old, 1000) && !session.cancel(old));
        CHECK(session.state() == State::AwaitIdle && session.error() == Error::None);
    } else if (mode == "busy_begin") {
        const auto token = session.begin(4,2048,128,0,500);
        const auto config = start(session, token);
        CHECK(!session.begin(0,256,8,1,500));
        CHECK(session.generation() == token && session.state() == State::Converting);
        CHECK(session.observeStatus(token, config & 0x7fff, 2));
        CHECK(session.observeStatus(token, config, 3));
        CHECK(session.observeSample(token, 16384, 4));
        CHECK(session.sample().volts == 1.024);
    } else if (mode == "timeout_phases") {
        for (unsigned phase = 0; phase < 5; ++phase) {
            const auto token = session.begin(4,2048,128,100,50);
            if (phase == 1) CHECK(session.observeStatus(token, 0x42e3, 101));
            if (phase >= 2) CHECK(session.observeStatus(token, 0x8583, 101));
            if (phase >= 3) {
                const auto config = session.writeValue();
                CHECK(session.acknowledgeWrite(token, true, 102));
                CHECK(session.observeStatus(token, config & 0x7fff, 103));
                if (phase == 4) CHECK(session.observeStatus(token, config, 104));
            }
            CHECK(session.tick(token, 149)); CHECK(!session.tick(token, 150));
            CHECK(session.error() == Error::Timeout && !session.sample().valid);
        }
    } else if (mode == "timer_wrap") {
        const auto token = session.begin(4,2048,128,UINT32_MAX - 9,50);
        const auto config = start(session, token, UINT32_MAX - 8);
        CHECK(session.observeStatus(token, config & 0x7fff, UINT32_MAX - 7));
        CHECK(session.observeStatus(token, config, 1));
        CHECK(session.observeSample(token, 0, 2) && session.sampleTimeMs() == 2);
        const auto second = session.begin(4,2048,128,UINT32_MAX - 9,50);
        CHECK(session.tick(second,39)); CHECK(!session.tick(second,40));
        CHECK(session.error() == Error::Timeout);
    } else if (mode == "transport_failures") {
        for (unsigned phase = 0; phase < 5; ++phase) {
            const auto token = session.begin(4,2048,128,0,500);
            if (phase == 1) CHECK(session.observeStatus(token, 0x42e3, 1));
            if (phase >= 2) CHECK(session.observeStatus(token, 0x8583, 1));
            if (phase >= 3) {
                const auto config = session.writeValue();
                CHECK(session.acknowledgeWrite(token, true, 2));
                CHECK(session.observeStatus(token, config & 0x7fff, 3));
                if (phase == 4) CHECK(session.observeStatus(token, config, 4));
            }
            CHECK(session.transportFailed(token, 5));
            CHECK(session.error() == Error::Transport && session.operation() == Operation::None);
            CHECK(!session.observeSample(token, 123, 6));
        }
    } else if (mode == "write_nack") {
        for (const uint16_t previous : {uint16_t(0x42e3),uint16_t(0x8583)}) {
            const auto token = session.begin(4,2048,128,0,500);
            CHECK(session.observeStatus(token, previous, 1));
            CHECK(!session.acknowledgeWrite(token, false, 2));
            CHECK(session.error() == Error::Transport && !session.sample().valid);
        }
    } else if (mode == "invalid_config") {
        CHECK(!session.begin(8,2048,128,0,500) && session.error() == Error::InvalidConfig);
        CHECK(!session.begin(4,2000,128,0,500) && session.error() == Error::InvalidConfig);
        CHECK(!session.begin(4,2048,130,0,500) && session.error() == Error::InvalidConfig);
        for (unsigned timeout : {0U,1001U,UINT32_MAX})
            CHECK(!session.begin(4,2048,128,0,timeout) && session.error() == Error::InvalidTimeout);
    } else if (mode == "rails_and_restart") {
        for (const uint16_t code : {uint16_t(0x7fff),uint16_t(0x8000)}) {
            const auto token = session.begin(4,2048,128,0,500);
            const auto config = start(session, token);
            CHECK(session.observeStatus(token, config & 0x7fff, 1));
            CHECK(session.observeStatus(token, config, 2));
            CHECK(session.observeSample(token, code, 3));
            CHECK(session.sample().atCodeRail && session.sample().valid);
            CHECK(!session.observeSample(token, 0, 4));
            CHECK(session.sample().atCodeRail && session.sampleTimeMs() == 3);
        }
        CHECK(!session.begin(8,2048,128,0,500));
        CHECK(!session.sample().valid && std::isnan(session.sample().volts) && session.sampleTimeMs() == 0);
    } else if (mode == "out_of_order") {
        const auto token = session.begin(4,2048,128,0,500);
        CHECK(!session.observeSample(token, 0, 1));
        CHECK(!session.acknowledgeWrite(token, true, 1));
        CHECK(session.state() == State::AwaitIdle && !session.sample().valid);
        const auto config = start(session, token);
        CHECK(!session.observeSample(token, 0, 1));
        CHECK(session.observeStatus(token, config & 0x7fff, 1));
        CHECK(session.observeStatus(token, config, 2));
        CHECK(!session.observeStatus(token, 0, 3));
        CHECK(!session.acknowledgeWrite(token, true, 3));
        CHECK(session.state() == State::SamplePending);
    } else CHECK(false);
    std::printf("%s PASS (protocol session only; no driver/device)\n", mode.c_str());
}
