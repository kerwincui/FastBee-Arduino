#include "core/MotorTravelAdmission.h"
#include "core/MotorTravelInterlock.h"
#include <array>
#include <cassert>
#include <cstring>
#include <vector>
#include <climits>

using FastBee::MotorTravelAdmission;
using FastBee::MotorTravelMapping;

static MotorTravelMapping mapping(uint8_t slave = 8) {
    MotorTravelMapping p;
    p.required = p.valid = p.finite = true;
    p.slave = slave; p.readFunction = 3; p.writeFunction = 6;
    const uint16_t regs[] = {0, 1, 2, 5, 7};
    for (unsigned i = 0; i < 5; ++i) p.regs[i] = regs[i];
    for (unsigned i = 0; i < 3; ++i) p.values[i] = 1;
    p.pulseReadRegister = 7; p.minimum = 0; p.maximum = 1000;
    return p;
}
static std::vector<uint8_t> frame(uint8_t slave, uint8_t fc, uint16_t reg, uint16_t value) {
    std::vector<uint8_t> result = {slave, fc, static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg),
        static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value), 0, 0};
    if (fc == 16) result = {slave, fc, result[2], result[3], 0, 1, 2, result[4], result[5], 0, 0};
    return result; // CRC belongs to the real transport, not this pure admission test.
}
static void readPulse(MotorTravelAdmission& axis, uint16_t pulse = 100, uint32_t now = 100) {
    const auto& m = axis.mapping;
    auto request = frame(m.slave, m.readFunction, m.pulseReadRegister, 1);
    const uint8_t response[] = {m.slave, m.readFunction, 2, static_cast<uint8_t>(pulse >> 8), static_cast<uint8_t>(pulse), 0, 0};
    assert(!axis.before(request.data(), request.size(), now));
    axis.after(request.data(), request.size(), response, sizeof(response), true, now);
    assert(axis.pulseKnown && axis.pulse == pulse);
}
static MotorTravelAdmission referenced(MotorTravelMapping m = mapping()) {
    MotorTravelAdmission a;
    a.configure(m);
    assert(a.confirmPosition(500, true));
    readPulse(a);
    return a;
}
static bool send(MotorTravelAdmission& axis, const std::vector<uint8_t>& request,
                 unsigned& writes, uint32_t now = 100, bool acknowledged = true) {
    if (axis.before(request.data(), request.size(), now)) return false;
    ++writes;
    std::array<uint8_t, 8> reply{};
    for (unsigned i = 0; i < 6; ++i) reply[i] = request[i];
    axis.after(request.data(), request.size(), reply.data(), reply.size(), acknowledged, now);
    return true;
}

int main() {
    auto fwd = frame(8, 6, 0, 1), rev = frame(8, 6, 1, 1), stop = frame(8, 6, 2, 1);
    {
        auto commandMap = mapping(); commandMap.commandPosition = true;
        commandMap.minimum = -400; commandMap.maximum = 400;
        MotorTravelAdmission commandAxis; commandAxis.configure(commandMap);
        assert(commandAxis.confirmPosition(0, true));
        unsigned sent = 0;
        for (unsigned cycle = 0; cycle < 100; ++cycle) {
            readPulse(commandAxis);
            assert(send(commandAxis, fwd, sent));
            assert(commandAxis.position.lower == 100 && commandAxis.position.upper == 100);
            assert(send(commandAxis, stop, sent)); readPulse(commandAxis);
            assert(send(commandAxis, rev, sent));
            assert(commandAxis.position.lower == 0 && commandAxis.position.upper == 0);
            assert(send(commandAxis, stop, sent));
        }
        for (unsigned step = 0; step < 4; ++step) {
            readPulse(commandAxis); assert(send(commandAxis, fwd, sent));
            assert(send(commandAxis, stop, sent));
        }
        readPulse(commandAxis);
        const unsigned before = sent;
        assert(!send(commandAxis, fwd, sent) && sent == before);
        assert(send(commandAxis, rev, sent) && commandAxis.position.lower == 300);
        assert(send(commandAxis, stop, sent)); readPulse(commandAxis);
        assert(send(commandAxis, rev, sent, 100, false));
        assert(!commandAxis.position.referenced && !send(commandAxis, rev, sent));
        assert(commandAxis.confirmPosition(0, true));
        commandMap.commandPosition = false; commandAxis.configure(commandMap);
        assert(!commandAxis.position.referenced && !commandAxis.position.commandTracking);
    }
    unsigned writes = 0;
    {
        auto startup = mapping(); startup.minimum = -400; startup.maximum = 400;
        startup.commandPosition = startup.startupReference = true; startup.startupPosition = 0;
        MotorTravelAdmission boot;
        boot.configure(startup);
        assert(boot.position.referenced && boot.position.lower == 0);
        assert(!boot.stationary && !boot.pulseKnown && !boot.stopAcknowledged);
        readPulse(boot);
        assert(!send(boot, fwd, writes));
        assert(send(boot, stop, writes)); readPulse(boot);
        assert(send(boot, fwd, writes) && boot.position.lower == 100);
        boot.configure(startup);
        assert(boot.position.lower == 100);
        assert(send(boot, stop, writes)); readPulse(boot);
        assert(send(boot, rev, writes, 100, false));
        boot.configure(startup);
        assert(!boot.position.referenced && !send(boot, fwd, writes));
        boot = {}; boot.configure(startup);
        assert(boot.position.referenced && boot.position.lower == 0 && !boot.stationary);
        boot.invalidate(); boot.configure(startup);
        assert(!boot.position.referenced);
        startup.startupPosition = 401; boot = {}; boot.configure(startup);
        assert(!boot.position.referenced);
        writes = 0;
    }
    MotorTravelAdmission a;
    a.configure(mapping());
    assert(!send(a, fwd, writes) && !send(a, rev, writes) && writes == 0);
    assert(!a.confirmPosition(500, false));
    assert(!a.confirmPosition(-1, true));
    assert(!a.confirmStationary(true));
    assert(send(a, stop, writes) && writes == 1 && !a.position.referenced);
    assert(send(a, frame(8, 6, 5, 10), writes));
    assert(send(a, frame(8, 6, 7, 100), writes));
    readPulse(a);
    assert(!a.position.referenced && !a.stationary);
    assert(!send(a, fwd, writes) && !send(a, rev, writes));

    // Default point-to-point jogs require no travel setup or reference. STOP,
    // valid finite parameters, protocol checks and single-submit behavior remain.
    for (uint8_t fc : {6, 16}) {
        auto manual = mapping(19); manual.writeFunction = fc;
        manual.minimum = manual.maximum = 0;
        a = {}; a.configure(manual); writes = 0;
        auto halt = frame(19, fc, 2, 1), start = frame(19, fc, 0, 1);
        assert(!a.position.referenced && !send(a, start, writes));
        assert(send(a, halt, writes)); readPulse(a);
        assert(send(a, start, writes) && !a.position.referenced);
        assert(send(a, halt, writes)); // Fresh parameters survive an acknowledged STOP.
        assert(send(a, frame(19, fc, 1, 1), writes));
        assert(send(a, halt, writes));
        assert(send(a, start, writes, 100, false));
        assert(!send(a, start, writes));
        assert(send(a, halt, writes)); readPulse(a);
        assert(send(a, start, writes)); // New explicit request; no invented position.
    }

    // Commissioning parameters must work before travel calibration, including
    // non-finite profiles. Distinct command registers and STOP still matter.
    auto uncalibrated = mapping(); uncalibrated.maximum = uncalibrated.minimum;
    uncalibrated.finite = false;
    a = {}; a.configure(uncalibrated); writes = 0;
    assert(!send(a, frame(8, 6, 7, 100), writes) && writes == 0);
    assert(send(a, stop, writes));
    assert(send(a, frame(8, 6, 7, 100), writes));
    assert(!send(a, fwd, writes) && !a.position.referenced);
    uncalibrated.regs[3] = uncalibrated.regs[0];
    a.configure(uncalibrated);
    assert(send(a, stop, writes));
    assert(!send(a, frame(8, 6, uncalibrated.regs[3], 10), writes));

    a = referenced(); writes = 0;
    assert(send(a, fwd, writes) && writes == 1);
    assert(a.position.lower == 500 && a.position.upper == 600 && !a.stationary);
    assert(!send(a, fwd, writes) && !send(a, rev, writes) && writes == 1);
    assert(!a.confirmStationary(false));
    assert(a.confirmStationary(true));
    assert(a.position.lower == 500 && a.position.upper == 600);
    assert(send(a, rev, writes));
    assert(a.position.lower == 400 && a.position.upper == 600);
    assert(send(a, stop, writes));
    assert(a.position.referenced && !a.stationary && a.pulseKnown);
    assert(a.position.lower == 400 && a.position.upper == 600);
    assert(a.stopAcknowledged);
    readPulse(a);
    assert(!a.stationary); // Acknowledged stop is command state, not motion feedback.
    assert(send(a, fwd, writes));
    assert(a.position.lower == 400 && a.position.upper == 700);
    assert(!a.stopAcknowledged);

    a = referenced(); writes = 0;
    assert(send(a, stop, writes)); // An idle stop must not erase the origin.
    assert(a.position.referenced && a.stationary && a.pulseKnown);
    readPulse(a);
    assert(send(a, rev, writes));

    a = referenced(); writes = 0;
    assert(send(a, stop, writes, 100, false));
    assert(a.position.referenced && !a.stationary && !a.stopAcknowledged && !a.pulseKnown);
    readPulse(a);
    assert(!send(a, fwd, writes)); // A successful read cannot replace the missing STOP acknowledgement.
    assert(send(a, stop, writes)); readPulse(a);
    assert(send(a, fwd, writes));

    // STOP and parameter failures cannot initiate a move. Keep the existing
    // coordinate/bounds, require another STOP and readback, and never rebase.
    for (uint8_t fc : {6, 16}) for (bool commanded : {false, true}) {
        auto recoveryMap = mapping(19); recoveryMap.writeFunction = fc;
        recoveryMap.commandPosition = commanded;
        for (unsigned index : {2U, 3U, 4U}) for (bool badEcho : {false, true}) {
            a = referenced(recoveryMap); writes = 0;
            auto move = frame(19, fc, recoveryMap.regs[0], recoveryMap.values[0]);
            auto halt = frame(19, fc, recoveryMap.regs[2], recoveryMap.values[2]);
            assert(send(a, move, writes));
            assert(send(a, halt, writes));
            const auto lower = a.position.lower, upper = a.position.upper;
            auto request = frame(19, fc, recoveryMap.regs[index], index == 2 ? recoveryMap.values[2] : 150);
            assert(!a.before(request.data(), request.size(), 100));
            std::array<uint8_t, 8> reply{};
            std::copy(request.begin(), request.begin() + 6, reply.begin());
            reply[3] ^= 1;
            a.after(request.data(), request.size(), badEcho ? reply.data() : nullptr,
                    badEcho ? reply.size() : 0, badEcho, 100);
            assert(a.position.referenced && a.position.lower == lower && a.position.upper == upper);
            assert(!a.stationary && !a.stopAcknowledged && !a.pulseKnown);
            readPulse(a, 150);
            assert(!send(a, move, writes));
            assert(send(a, halt, writes)); readPulse(a, 150);
            assert(send(a, move, writes));
            // An ambiguous START still loses the reference permanently until
            // explicit confirmation; subsequent STOP/readback cannot restore it.
            assert(send(a, halt, writes)); readPulse(a, 50);
            assert(send(a, move, writes, 100, false));
            assert(!a.position.referenced);
            assert(send(a, halt, writes)); readPulse(a, 50);
            assert(!a.position.referenced && !send(a, move, writes));
        }
    }

    a = referenced(); writes = 0;
    assert(send(a, fwd, writes, 100, false)); // Lost reply may mean it ran.
    assert(!a.position.referenced && a.position.upper == 600);
    assert(!send(a, rev, writes) && writes == 1);

    a = referenced(); writes = 0;
    assert(send(a, frame(8, 6, 7, 150), writes));
    assert(!a.pulseKnown && !send(a, fwd, writes)); // A write echo is not parameter readback.
    readPulse(a, 150);
    assert(send(a, fwd, writes));
    assert(!send(a, frame(8, 6, 5, 200), writes)); // No parameter edit while motion is unknown.

    a = referenced(); writes = 0;
    assert(!send(a, fwd, writes, 15101) && writes == 0);
    readPulse(a, 100, UINT32_MAX - 5);
    assert(send(a, fwd, writes, 9)); // millis wrap is handled, not a negative age.

    a = referenced(); writes = 0;
    auto changed = mapping(); changed.maximum = 900;
    a.configure(changed);
    assert(!send(a, fwd, writes) && writes == 0);
    a = MotorTravelAdmission{}; a.configure(mapping());
    assert(!send(a, fwd, writes)); // Reboot does not restore the saved estimate.

    a = referenced(); writes = 0;
    assert(a.confirmPosition(950, true)); readPulse(a, 100);
    assert(!send(a, fwd, writes) && writes == 0 && a.position.upper == 950);
    assert(send(a, rev, writes));
    assert(a.position.lower == 850 && a.position.upper == 950);

    a = referenced(); writes = 0;
    for (const auto& blocked : {frame(0, 6, 0, 1), frame(8, 5, 0, 0xff00), frame(8, 23, 0, 1),
                               frame(8, 6, 99, 1), frame(8, 6, 0, 9), frame(8, 16, 0, 1)})
        assert(!send(a, blocked, writes));
    assert(writes == 0);
    auto unrelated = frame(6, 5, 0, 0xff00);
    assert(!a.before(unrelated.data(), unrelated.size(), 100)); // Other relay remains independent.

    auto generic = mapping(19); generic.writeFunction = 16; generic.readFunction = 4;
    generic.regs[0] = generic.regs[1] = generic.regs[2] = 300;
    generic.regs[3] = 301; generic.regs[4] = 302;
    generic.values[0] = 17; generic.values[1] = 34; generic.values[2] = 0;
    generic.pulseReadRegister = 400;
    a = referenced(generic); writes = 0;
    assert(send(a, frame(19, 16, 300, 17), writes));
    assert(!send(a, frame(19, 16, 301, 30), writes));
    assert(send(a, frame(19, 16, 300, 0), writes)); // stop=0 works without model-specific rules.
    auto bulk = frame(19, 16, 300, 0); bulk[5] = 2; bulk[6] = 4;
    bulk.insert(bulk.end() - 2, {0, 1});
    assert(!send(a, bulk, writes)); // Combined stop/start write is never admitted as stop.

    a = referenced(); auto badRead = frame(8, 3, 7, 1);
    const uint8_t wrong[] = {8, 3, 4, 0, 100, 0, 0};
    a.after(badRead.data(), badRead.size(), wrong, sizeof(wrong), true, 100);
    assert(!a.pulseKnown && a.position.referenced);
    assert(a.checkStart(true, 100, 100));
    auto readOverflow = frame(8, 3, 65535, 2);
    assert(a.before(readOverflow.data(), readOverflow.size(), 100));

    changed = mapping(); changed.regs[2] = changed.regs[0];
    a.configure(changed); assert(!a.confirmPosition(500, true));
    writes = 0; assert(!send(a, frame(8, 6, 0, 1), writes) && writes == 0);
    changed = mapping(); changed.finite = false;
    a.configure(changed); assert(!a.confirmPosition(500, true));
    assert(send(a, stop, writes));
    FastBee::MotorTravelInterlock<3> bank;
    bank.configure(0, mapping());
    auto generation = bank.generation;
    assert(bank.confirm(0, generation, true, 500, true) == nullptr);
    assert(bank.confirm(0, generation, true, 500, true)); // Replayed confirmation is rejected.
    assert(!bank.axes[0].pulseKnown);
    readPulse(bank.axes[0]);
    generation = bank.generation;
    assert(!bank.before(fwd.data(), fwd.size(), 100));
    assert(bank.confirm(0, generation, false, 0, true)); // A write invalidates an old UI snapshot.
    assert(!bank.confirm(0, bank.generation, false, 0, true));
    assert(bank.axes[0].position.lower == 500 && bank.axes[0].position.upper == 600);
    bank.reset();
    assert(!bank.axes[0].position.referenced);
    assert(bank.before(fwd.data(), fwd.size(), 100));
    bank.configure(1, mapping());
    assert(!strcmp(bank.before(fwd.data(), fwd.size(), 100), "TRAVEL_DUPLICATE_SLAVE"));
    assert(bank.confirm(2, bank.generation, true, 0, true));
    assert(bank.protects(8) && bank.protects(0) && !bank.protects(6));
    bank.configure(1, FastBee::MotorTravelMapping{});
    bank.axes[0] = referenced(); generation = bank.generation;
    const auto failedSpeed = frame(8, 3, 5, 1);
    bank.after(failedSpeed.data(), failedSpeed.size(), nullptr, 0, false, 100);
    assert(bank.axes[0].position.referenced && !bank.axes[0].pulseKnown && bank.generation != generation);
    assert(bank.confirm(0, generation, true, 500, true));
    assert(bank.axes[0].position.lower == 500 && bank.axes[0].position.upper == 500);
    readPulse(bank.axes[0]);
    assert(!bank.axes[0].checkStart(true, 100, 100));
}
