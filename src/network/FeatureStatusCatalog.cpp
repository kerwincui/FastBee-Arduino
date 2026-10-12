#include "network/FeatureStatusCatalog.h"
#include "utils/PsramJsonDocument.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace FastBee {
namespace {
enum Flag : uint16_t {
    Compiled = 1U << 0, UserEnabled = 1U << 1, BudgetPassed = 1U << 2,
    Active = 1U << 3, InstanceObserved = 1U << 4, HasCount = 1U << 5,
    RuntimeDisabled = 1U << 6, RequiresRestart = 1U << 7,
    BootEnabled = 1U << 8, PendingRestart = 1U << 9, OnDemand = 1U << 10,
    BundleRequired = 1U << 11, Deferred = 1U << 12, Locked = 1U << 13
};
const struct { const char* key; Flag flag; } FLAG_FIELDS[] = {
    {"compiled", Compiled}, {"userEnabled", UserEnabled}, {"budgetPassed", BudgetPassed},
    {"active", Active}, {"instanceObserved", InstanceObserved}, {"runtimeDisabled", RuntimeDisabled},
    {"requiresRestart", RequiresRestart}, {"bootEnabled", BootEnabled},
    {"pendingRestart", PendingRestart}, {"onDemand", OnDemand},
    {"bundleRequired", BundleRequired}, {"deferred", Deferred}, {"locked", Locked}
};
// Resolve copied AJ strings to static storage, never borrow document memory.
const char* canonical(const char* value) {
    static const char* const VALUES[] = {
        "admitted", "not_compiled", "dependency_blocked", "restart_required",
        "user_disabled", "budget_denied", "runtime_disabled", "waiting_for_network",
        "dependency_missing", "dependency_cycle", "unobserved", "not_initialized",
        "initialized", "busy", "cleanup_pending", "allocated", "running", "stopped",
        "retiring", "idle", "owner_absent", "driver_initialization", "service_lifecycle",
        "execution_activity", "static_resource", "owner_lifetime", "transport_allocation",
        "observer_not_registered", "synchronization_unavailable", "backend_unavailable",
        "invalid_snapshot", "observation_busy", "no_dedicated_instance",
        "owner_snapshot_unavailable", "implementation_missing", ""
    };
    if (value) for (const char* entry : VALUES) if (strcmp(value, entry) == 0) return entry;
    return nullptr;
}

class Writer {
public:
    Writer(char* data, size_t capacity) : data_(data), capacity_(capacity) {}
    void byte(char value) {
        if (length_ < capacity_) data_[length_++] = value;
        else valid_ = false;
    }
    void raw(const char* value) { while (*value) byte(*value++); }
    void text(const char* value) {
        if (!value) { raw("null"); return; }
        byte('"');
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
            if (*p == '"' || *p == '\\') { byte('\\'); byte(static_cast<char>(*p)); }
            else if (*p < 0x20) {
                static const char hexDigits[] = "0123456789abcdef";
                byte('\\'); raw("u00"); byte(hexDigits[*p >> 4]); byte(hexDigits[*p & 15]);
            } else byte(static_cast<char>(*p));
        }
        byte('"');
    }
    void number(uint32_t value) {
        char buffer[11];
        const int size = snprintf(buffer, sizeof(buffer), "%lu", static_cast<unsigned long>(value));
        if (size < 0 || static_cast<size_t>(size) >= sizeof(buffer)) { valid_ = false; return; }
        raw(buffer);
    }
    void key(const char* name) { byte(','); text(name); byte(':'); }
    void textField(const char* name, const char* value) { key(name); text(value); }
    void numberField(const char* name, uint32_t value) { key(name); number(value); }
    void flagField(const char* name, bool value) { key(name); raw(value ? "true" : "false"); }
    bool valid() const { return valid_; }
    size_t size() const { return length_; }
private:
    char* data_;
    size_t capacity_, length_ = 0;
    bool valid_ = true;
};
} // namespace

FeatureStatusCatalog::FeatureStatusCatalog(const RuntimeFeatureManager& manager)
    : count_(manager.featureCount()) {
    if (count_ > RuntimeFeatureManager::kMaxFeatures) return;
    if (count_) {
        snapshots_.reset(new (std::nothrow) Snapshot[count_]);
        if (!snapshots_) return;
    }
    // Retain the existing status/admission implementation as the single source
    // of truth. Only one small AJ document exists during capture, before send.
    for (size_t i = 0; i < count_; ++i) {
        auto document = makeJsonDocument(1024);
        JsonObject object = document.to<JsonObject>();
        if (!manager.getFeatureStatusJson(i, object) || document.overflowed()) return;
        Snapshot& snapshot = snapshots_[i];
        const auto* meta = manager.getFeatureAt(i);
        if (!meta) return;
        // Copy static-lifetime string pointers, not a pointer to mutable state.
        snapshot.key = meta->key;
        snapshot.name = meta->displayName;
        snapshot.group = meta->group;
        snapshot.hwDep = meta->hwDep;
        snapshot.dependsOn = meta->dependsOn;
        snapshot.admission = canonical(object["admissionState"].as<const char*>());
        snapshot.instanceState = canonical(object["instanceState"].as<const char*>());
        snapshot.instanceScope = canonical(object["instanceScope"].as<const char*>());
        snapshot.instanceReason = canonical(object["instanceReason"].as<const char*>());
        if (!snapshot.admission || !snapshot.instanceState || !snapshot.instanceScope ||
            !snapshot.instanceReason) return;
        for (const auto& field : FLAG_FIELDS)
            if (object[field.key].as<bool>()) snapshot.flags |= field.flag;
        snapshot.flashKB = object["flashKB"].as<uint32_t>();
        snapshot.ramB = object["ramB"].as<uint32_t>();
        if (object["initializedInstances"].is<uint16_t>()) {
            snapshot.flags |= HasCount;
            snapshot.initializedCount = object["initializedInstances"].as<uint16_t>();
        }
        const char* reason = object["runtimeDisableReason"].as<const char*>();
        if (reason) {
            const size_t length = strlen(reason);
            if (length >= 96) return; // RuntimeFeatureManager's bounded diagnostic size.
            snapshot.runtimeReason.reset(new (std::nothrow) char[length + 1]);
            if (!snapshot.runtimeReason) return;
            memcpy(snapshot.runtimeReason.get(), reason, length + 1);
        }
    }
    auto document = makeJsonDocument(1024);
    JsonObject summary = document.to<JsonObject>();
    manager.getStatusSummaryJson(summary);
    if (document.overflowed()) return;
    totalFlashKB_ = summary["totalFlashKB"].as<uint32_t>();
    activeFlashKB_ = summary["activeFlashKB"].as<uint32_t>();
    activeRamB_ = summary["activeRamB"].as<uint32_t>();
    runtimeDegraded_ = summary["runtimeDegraded"].as<bool>();
    hasHeap_ = summary["free8KB"].is<uint32_t>() && summary["largest8KB"].is<uint32_t>();
    free8KB_ = summary["free8KB"].as<uint32_t>();
    largest8KB_ = summary["largest8KB"].as<uint32_t>();
    valid_ = true;
}

void FeatureStatusCatalog::rewind() { phase_ = 0; record_ = length_ = offset_ = 0; }

bool FeatureStatusCatalog::nextFragment() {
    if (!valid_ || phase_ == 3) return false;
    Writer writer(fragment_, sizeof(fragment_));
    if (phase_ == 0) {
        writer.raw("{\"success\":true,\"data\":{\"summary\":{\"totalFlashKB\":");
        writer.number(totalFlashKB_);
        writer.numberField("activeFlashKB", activeFlashKB_);
        writer.numberField("activeRamB", activeRamB_);
        writer.textField("resourceAccounting", "static_estimate");
        writer.flagField("instanceMemoryMeasured", false);
        writer.flagField("runtimeDegraded", runtimeDegraded_);
        if (hasHeap_) {
            writer.numberField("free8KB", free8KB_);
            writer.numberField("largest8KB", largest8KB_);
        }
        writer.raw("},\"features\":[");
        phase_ = 1;
    } else if (record_ < count_) {
        const Snapshot& snapshot = snapshots_[record_];
        if (record_) writer.byte(',');
        writer.raw("{\"key\":"); writer.text(snapshot.key);
        writer.textField("name", snapshot.name);
        writer.textField("group", snapshot.group);
        writer.flagField("compiled", snapshot.flags & Compiled);
        writer.flagField("userEnabled", snapshot.flags & UserEnabled);
        writer.flagField("budgetPassed", snapshot.flags & BudgetPassed);
        if (snapshot.flags & RequiresRestart) {
            writer.flagField("requiresRestart", true);
            writer.flagField("bootEnabled", snapshot.flags & BootEnabled);
            writer.flagField("pendingRestart", snapshot.flags & PendingRestart);
            writer.flagField("onDemand", snapshot.flags & OnDemand);
        }
        writer.flagField("active", snapshot.flags & Active);
        writer.textField("activeMeaning", "admission_only");
        writer.textField("admissionState", snapshot.admission);
        writer.textField("instanceState", snapshot.instanceState);
        writer.flagField("instanceObserved", snapshot.flags & InstanceObserved);
        writer.textField("instanceScope", snapshot.instanceScope);
        writer.textField("instanceReason", snapshot.instanceReason);
        if (snapshot.flags & HasCount) writer.numberField("initializedInstances", snapshot.initializedCount);
        writer.flagField("runtimeDisabled", snapshot.flags & RuntimeDisabled);
        if (snapshot.runtimeReason) writer.textField("runtimeDisableReason", snapshot.runtimeReason.get());
        writer.numberField("flashKB", snapshot.flashKB);
        writer.numberField("ramB", snapshot.ramB);
        if (snapshot.hwDep) writer.textField("hwDep", snapshot.hwDep);
        if (snapshot.dependsOn) writer.textField("dependsOn", snapshot.dependsOn);
        writer.flagField("bundleRequired", snapshot.flags & BundleRequired);
        if (snapshot.flags & Deferred) writer.flagField("deferred", true);
        if (snapshot.flags & Locked) {
            writer.flagField("locked", true);
            writer.textField("lockReason", strcmp(snapshot.key, "ota") == 0
                ? "当前分区不支持 OTA（需要带 ota_1 的 A/B 双分区）"
                : strcmp(snapshot.key, "otaFs") == 0 ? "未实现文件系统 OTA 写入"
                : "Not compiled into firmware");
        }
        writer.byte('}');
        ++record_;
    } else {
        writer.raw("]}}");
        phase_ = 3;
    }
    valid_ = writer.valid();
    length_ = valid_ ? writer.size() : 0;
    offset_ = 0;
    return valid_;
}

size_t FeatureStatusCatalog::measureLength() {
    if (measured_) return measuredLength_;
    rewind();
    size_t total = 0;
    while (nextFragment()) total += length_;
    rewind();
    measured_ = true;
    measuredLength_ = valid_ ? total : 0;
    return measuredLength_;
}

size_t FeatureStatusCatalog::read(uint8_t* destination, size_t capacity) {
    if (!destination || !valid_) return 0;
    if (!measured_ && !measureLength()) return 0;
    size_t written = 0;
    while (written < capacity) {
        if (offset_ == length_ && !nextFragment()) break;
        const size_t copied = std::min(capacity - written, length_ - offset_);
        memcpy(destination + written, fragment_ + offset_, copied);
        offset_ += copied;
        written += copied;
    }
    // Staged bytes are retained by AsyncAbstractResponse until ACK/FIN.
    if (phase_ == 3 && offset_ == length_) snapshots_.reset();
    return written;
}
} // namespace FastBee
