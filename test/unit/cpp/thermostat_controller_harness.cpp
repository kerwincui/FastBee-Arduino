#include "core/ThermostatController.h"
#include <cassert>
#include <cstring>
#include <limits>
using namespace FastBee;

static bool tick(ThermostatController& controller, const ThermostatSettings& cfg,
                 double value, uint64_t now, bool valid = true) {
    const bool command = controller.decide(cfg, {valid, value, now}, now);
    controller.confirm(command, now);
    return command;
}
int main(int argc, char** argv) {
    assert(argc == 2);
    const char* mode = argv[1];
    ThermostatSettings cfg;
    ThermostatController control;
    control.begin(1000);
    if (!std::strcmp(mode, "startup")) {
        assert(!control.decide(cfg, {true, 10, 999}, 1000));
        assert(!tick(control, cfg, 10, 1001, false));
        assert(!control.faulted()); // No cached/startup missing sample can arm or latch.
        assert(!tick(control, cfg, 10, 2000));
        assert(control.phase() == ThermostatPhase::MinimumOff);
        assert(tick(control, cfg, 10, 31000));
    } else if (!std::strcmp(mode, "heating")) {
        assert(tick(control, cfg, 24, 31000));
        assert(tick(control, cfg, 24.99, 62000));
        assert(!tick(control, cfg, 25, 63000));
        assert(!tick(control, cfg, 24.5, 100000));
        assert(tick(control, cfg, 24, 101000));
    } else if (!std::strcmp(mode, "cooling")) {
        cfg.cooling = true;
        assert(tick(control, cfg, 26, 31000));
        assert(tick(control, cfg, 25.01, 62000));
        assert(!tick(control, cfg, 25, 63000));
        assert(!tick(control, cfg, 25.5, 100000));
        assert(tick(control, cfg, 26, 101000));
    } else if (!std::strcmp(mode, "dwell")) {
        assert(tick(control, cfg, 20, 31000));
        assert(tick(control, cfg, 30, 31001));
        assert(control.phase() == ThermostatPhase::MinimumOn);
        assert(!tick(control, cfg, 30, 61000));
        assert(!tick(control, cfg, 20, 61001));
        assert(tick(control, cfg, 20, 91000));
    } else if (!std::strcmp(mode, "manual-fault")) {
        assert(tick(control, cfg, 20, 31000));
        assert(!tick(control, cfg, 20, 31001, false)); // Bypass minimum-on.
        assert(!tick(control, cfg, 20, 100000));
        assert(control.phase() == ThermostatPhase::ManualRecovery);
        control.begin(100000);
        assert(!control.decide(cfg, {true, 20, 100000}, 131000));
        assert(tick(control, cfg, 20, 131001));
    } else if (!std::strcmp(mode, "fresh-recovery")) {
        cfg.automaticRecovery = true;
        assert(tick(control, cfg, 20, 31000));
        assert(!tick(control, cfg, 20, 31001, false));
        assert(!control.decide(cfg, {true, 20, 31000}, 31002));
        assert(!tick(control, cfg, 20, 31003));
        assert(control.phase() == ThermostatPhase::MinimumOff);
        assert(tick(control, cfg, 20, 61001));
    } else if (!std::strcmp(mode, "stale-future-nan")) {
        assert(tick(control, cfg, 20, 31000));
        assert(!control.decide(cfg, {true, 20, 31000}, 61001));
        control.begin(1000);
        assert(!control.decide(cfg, {true, 20, 100001}, 100000));
        assert(!tick(control, cfg, std::numeric_limits<double>::quiet_NaN(), 100000));
    } else if (!std::strcmp(mode, "permanent-fault")) {
        cfg.automaticRecovery = true;
        assert(tick(control, cfg, 20, 31000));
        control.outputFailure(31001);
        assert(!tick(control, cfg, 20, 100000));
        assert(control.phase() == ThermostatPhase::OutputUnknown);
        control.begin(1000);
        control.fault(2000, ThermostatPhase::SourceChanged, true);
        assert(!tick(control, cfg, 20, 100000));
    } else if (!std::strcmp(mode, "monotonic-64bit")) {
        const uint64_t large = uint64_t(1) << 33;
        control.begin(large);
        assert(!tick(control, cfg, 20, large + 1));
        assert(tick(control, cfg, 20, large + 30000));
        assert(!control.decide(cfg, {true, 20, large + 30000}, large - 1));
    } else return 2;
    return 0;
}
