#pragma once
#include "systems/ConfigMigrationJournal.h"
#include <Arduino.h>
#include <atomic>

namespace FastBee { namespace ConfigMigration {
// One staging owner. HTTP handlers must hold this guard through each operation;
// reboot recovery runs before handlers/tasks and takes the same guard.
class Guard {
    static std::atomic_flag& gate() {static std::atomic_flag value=ATOMIC_FLAG_INIT;return value;}
    bool held;
public:
    Guard():held(!gate().test_and_set(std::memory_order_acquire)){}
    ~Guard(){if(held)gate().clear(std::memory_order_release);}
    explicit operator bool()const{return held;}
    Guard(const Guard&)=delete;Guard& operator=(const Guard&)=delete;
};
struct Selection {uint32_t fileId;uint32_t size;};
class Storage {
public:
    Phase phase() const;
    bool readManifest(Manifest& manifest) const;
    bool verifySlot(size_t index,Slot slot,const Fingerprint& expected) const;
    bool currentMatches(uint32_t fileId,bool backup,const Fingerprint& expected) const;
    bool firmwareMatches(const char* expected) const;
    bool install(size_t index,Slot slot,uint32_t fileId,bool backup,const Fingerprint& expected);
    bool mark(Phase phase);

    // Begin/copy/upload never writes an active configuration generation.
    bool begin(const char* session,const Selection* selected,size_t count,String& error);
    bool append(const char* session,uint32_t fileId,uint32_t offset,const uint8_t* data,size_t size,String& error);
    // Read-only comparison of browser-preserved rule bytes with this session's
    // immutable baseline; no original content is returned by the endpoint.
    bool matchesOriginalRange(const char* session,uint32_t fileId,uint32_t offset,uint32_t total,
        const uint8_t* data,size_t size,String& error) const;
    using Validator=bool (*)(const Manifest&,void*,String&);
    bool inspect(const char* session,Manifest& manifest,String& error) const;
    bool prepare(const char* session,const char* reviewedRevision,Validator validate,void* context,String& error);
    bool discard(const char* session,String& error);
    // Preserve an incomplete, never-prepared upload by renaming its entire
    // directory. Requires a fresh content digest and never deletes evidence.
    bool orphanRevision(String& revision,String& error) const;
    bool quarantineOrphan(const char* reviewedRevision,String& error);
    bool orphanRetained() const;
    bool occupied() const;
    static String revision(const Manifest& manifest);
    bool readReceiving(Manifest& manifest) const;
    bool readSession(Manifest& manifest) const;
    static String slotPath(size_t index,Slot slot);
    static bool protectedPath(const String& path);
};
// False blocks normal startup. Never auto-format or initialize drivers after an
// unconfirmed recovery; previous generations remain available for repair.
bool recoverAtBoot();
} }
