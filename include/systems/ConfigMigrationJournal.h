#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace FastBee {
// The journal operates only before runtime configuration is loaded. Storage
// supplies byte-verified copies and immutable markers; no driver callbacks occur.
namespace ConfigMigration {
constexpr size_t MaxFiles = 10;
constexpr size_t WritableFiles = 8;
constexpr uint32_t MaxFileBytes = 128 * 1024;
constexpr uint32_t MaxBundleBytes = 128 * 1024;
enum class Phase { None, Prepared, Applying, Committed, RolledBack, Conflict, Invalid, Blocked };
enum class Slot : uint8_t { Candidate, Original, Backup };
struct Fingerprint {
    uint32_t size = 0;
    uint8_t sha256[32] = {};
    uint8_t exists = 0;
    uint8_t reserved[3] = {};
};
struct Entry {
    uint32_t fileId = 0;
    Fingerprint candidate, original, backup;
};
struct Manifest {
    uint32_t format = 1;
    uint32_t count = 0;
    char session[33] = {};
    uint8_t reserved[3] = {};
    char firmware[33] = {}; // ESP sketch MD5; only this exact firmware may activate.
    uint8_t firmwareReserved[3] = {};
    Entry entries[MaxFiles];
};
static_assert(sizeof(Fingerprint) == 40 && sizeof(Entry) == 124 && sizeof(Manifest) == 1320,
              "Migration journal layout must be stable");

inline const char* fileName(uint32_t id) {
    static const char* const names[MaxFiles] = {"device.json", "features.json", "peripherals.json",
        "periph_exec.json", "rule_scripts.json", "screen_panel.json", "point_history.json", "ir_keys.json",
        "network.json", "protocol.json"}; // Read-only dependency generations.
    return id < MaxFiles ? names[id] : nullptr;
}
inline int fileId(const char* name) {
    if (!name) return -1;
    for (size_t i = 0; i < MaxFiles; ++i) if (!std::strcmp(name, fileName(i))) return static_cast<int>(i);
    return -1;
}
inline bool tokenValid(const char* token) {
    if (!token || std::strlen(token) != 32) return false;
    for (size_t i=0; i<32; ++i) if (!((token[i]>='0' && token[i]<='9') ||
        (token[i]>='a' && token[i]<='f'))) return false;
    return true;
}
inline bool same(const Fingerprint& a, const Fingerprint& b) {
    return a.exists==b.exists && a.size==b.size && !std::memcmp(a.sha256,b.sha256,32);
}
inline bool valid(const Fingerprint& value) {
    if (value.exists > 1 || value.size > MaxFileBytes) return false;
    if (value.exists) return true; // Originals may legitimately be empty/corrupt.
    const Fingerprint absent;
    return same(value, absent);
}
inline bool valid(const Manifest& manifest) {
    if (manifest.format!=1 || !manifest.count || manifest.count>MaxFiles ||
        manifest.session[32]!='\0' || !tokenValid(manifest.session) ||
        manifest.firmware[32]!='\0' || !tokenValid(manifest.firmware)) return false;
    uint32_t total=0, mask=0, selected=0;
    for (size_t i=0;i<manifest.count;++i) {
        const auto& row=manifest.entries[i];
        if (!fileName(row.fileId) || (mask & (1U<<row.fileId)) || !valid(row.candidate) ||
            !valid(row.original) || !valid(row.backup)) return false;
        if (row.candidate.exists) {
            if (row.fileId>=WritableFiles || !row.candidate.size) return false;
            ++selected;
        }
        mask|=1U<<row.fileId;total+=row.candidate.size;
    }
    return selected && manifest.count==MaxFiles && mask==((1U<<MaxFiles)-1) && total<=MaxBundleBytes;
}

// Store contract: verifySlot/current never writes; install copies a verified
// slot atomically or restores explicit absence; mark is immutable and checked.
template<class Store>
Phase rollback(Store& store, const Manifest& manifest) {
    // Verify every retained generation before the first recovery mutation.
    for (size_t i=0;i<manifest.count;++i)
        if (!store.verifySlot(i,Slot::Original,manifest.entries[i].original) ||
            !store.verifySlot(i,Slot::Backup,manifest.entries[i].backup)) return Phase::Blocked;
    for (size_t i=0;i<manifest.count;++i) {
        const auto& row=manifest.entries[i];
        if (!row.candidate.exists) continue;
        if (!store.install(i,Slot::Original,row.fileId,false,row.original) ||
            !store.install(i,Slot::Backup,row.fileId,true,row.backup)) return Phase::Blocked;
    }
    return store.mark(Phase::RolledBack) ? Phase::RolledBack : Phase::Blocked;
}

template<class Store>
Phase recoverBeforeLoad(Store& store) {
    const Phase phase=store.phase();
    if (phase==Phase::None || phase==Phase::Committed || phase==Phase::RolledBack ||
        phase==Phase::Conflict || phase==Phase::Invalid) return phase;
    if (phase==Phase::Blocked) return phase;
    Manifest manifest;
    if (!store.readManifest(manifest) || !valid(manifest)) return Phase::Blocked;
    if (phase==Phase::Applying) return rollback(store,manifest);
    if (phase!=Phase::Prepared) return Phase::Blocked;
    if (!store.firmwareMatches(manifest.firmware))
        return store.mark(Phase::Conflict)?Phase::Conflict:Phase::Blocked;
    for (size_t i=0;i<manifest.count;++i) {
        const auto& row=manifest.entries[i];
        if ((row.candidate.exists && !store.verifySlot(i,Slot::Candidate,row.candidate)) ||
            !store.verifySlot(i,Slot::Original,row.original) || !store.verifySlot(i,Slot::Backup,row.backup))
            return store.mark(Phase::Invalid)?Phase::Invalid:Phase::Blocked;
        if (!store.currentMatches(row.fileId,false,row.original) ||
            !store.currentMatches(row.fileId,true,row.backup))
            return store.mark(Phase::Conflict)?Phase::Conflict:Phase::Blocked;
    }
    if (!store.mark(Phase::Applying)) return Phase::Blocked;
    for (size_t i=0;i<manifest.count;++i) {
        const auto& row=manifest.entries[i];
        if (!row.candidate.exists) continue;
        // Backup must belong to the same candidate graph after commit. Retain
        // the previous primary AND backup separately until explicit cleanup.
        if (!store.install(i,Slot::Candidate,row.fileId,false,row.candidate) ||
            !store.install(i,Slot::Candidate,row.fileId,true,row.candidate)) return rollback(store,manifest);
    }
    // Never infer committed from partially matching active files. A lost commit
    // marker rolls the whole transaction back on the next boot.
    return store.mark(Phase::Committed) ? Phase::Committed : Phase::Blocked;
}
} // namespace ConfigMigration
} // namespace FastBee
