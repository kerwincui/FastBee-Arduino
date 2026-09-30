#include "core/MotorTravelAdmission.h"
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
    unsigned writes = 0;
    MotorTravelAdmission a;
    a.configure(mapping());
    assert(!send(a, fwd, writes) && !send(a, rev, writes) && writes == 0);
    assert(!a.confirmPosition(500, false));
    assert(!a.confirmPosition(-1, true));
    assert(!a.confirmStationary(true));
    assert(send(a, stop, writes) && writes == 1 && !a.position.referenced);

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
    assert(!a.position.referenced && !a.confirmStationary(true));
    readPulse(a);
    assert(!send(a, fwd, writes)); // Readback never restores a reference.

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
    assert(!a.pulseKnown);
    auto readOverflow = frame(8, 3, 65535, 2);
    assert(a.before(readOverflow.data(), readOverflow.size(), 100));

    changed = mapping(); changed.regs[2] = changed.regs[0];
    a.configure(changed); assert(!a.confirmPosition(500, true));
    writes = 0; assert(!send(a, frame(8, 6, 0, 1), writes) && writes == 0);
    changed = mapping(); changed.finite = false;
    a.configure(changed); assert(!a.confirmPosition(500, true));
    assert(send(a, stop, writes));
}
