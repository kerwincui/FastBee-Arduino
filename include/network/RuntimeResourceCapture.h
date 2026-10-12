#pragma once

#include <ArduinoJson.h>
#include <memory>
#include <new>

namespace FastBee {

// Large fixed registries must not consume the HTTP task's stack. Observe one
// bounded copy at a time; its strings are copied by the serializer before the
// scratch is released. Allocation failure is unknown, never an empty registry.
template <typename Snapshot, typename Observe, typename Serialize>
void writeAllocatedResourceSnapshot(JsonObject data, Observe observe, Serialize serialize) {
    std::unique_ptr<Snapshot> snapshot(new (std::nothrow) Snapshot);
    if (!snapshot) {
        data.clear();
        data["status"] = "unknown";
        data["reason"] = "SNAPSHOT_ALLOCATION_FAILED";
        data["hardwareVerified"] = false;
        data["leaseCount"] = nullptr;
        data["leases"] = nullptr;
        return;
    }
    observe(*snapshot);
    serialize(data, *snapshot);
}

} // namespace FastBee
