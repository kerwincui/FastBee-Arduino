#pragma once
#include <cstdint>

namespace FastBee {
enum class TaskStackPermanentOwner : uint8_t { Unavailable, IdleCore0, IdleCore1, TimerDaemon, IpcCore0, IpcCore1 };
// Pointer equality only. A captured dynamic handle can already be dangling.
// The three reference handles must belong to permanent SDK tasks in this boot.
inline TaskStackPermanentOwner matchTaskStackPermanentOwner(const void* captured,
    const void* idle0, const void* idle1, const void* timer) {
    if (!captured) return TaskStackPermanentOwner::Unavailable;
    if (idle0 && captured == idle0) return TaskStackPermanentOwner::IdleCore0;
    if (idle1 && captured == idle1) return TaskStackPermanentOwner::IdleCore1;
    if (timer && captured == timer) return TaskStackPermanentOwner::TimerDaemon;
    return TaskStackPermanentOwner::Unavailable;
}
} // namespace FastBee
