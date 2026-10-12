#include "core/DacWriteAccess.h"
#include "core/ChipConfig.h"
#include "core/PeripheralManager.h"
#include "core/AsyncExecTypes.h"

namespace FastBee {
bool writeDacAtVersion(PeripheralManager& manager, const String& id,
                       uint32_t expectedVersion, uint8_t value) {
#if CHIP_HAS_DAC
    RecursiveMutexGuard lock(manager.getMutex(), 0);
    if (!lock.isLocked()) return false;
    PeripheralConfig current;
    uint32_t version = 0;
    if (!manager.getPeripheralSnapshot(id, current, &version) ||
        version != expectedVersion || current.type != PeripheralType::DAC || !current.enabled) return false;
    const auto* state = manager.getRuntimeState(id);
    if (!state || state->memorySuspended || state->memoryCleanupPending ||
        (state->status != PeripheralStatus::PERIPHERAL_INITIALIZED &&
         state->status != PeripheralStatus::PERIPHERAL_RUNNING)) return false;
    return manager.writeData(id, &value, 1);
#else
    (void)manager; (void)id; (void)expectedVersion; (void)value;
    return false;
#endif
}
}
