#include "systems/ConfigMigrationStorage.h"
#include <LittleFS.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>
#include <algorithm>
#include <cstring>

namespace FastBee { namespace ConfigMigration {
namespace {
constexpr const char* Root="/.config-migration";
constexpr const char* Ready="/.config-migration/ready.bin";
constexpr const char* Receiving="/.config-migration/receiving.bin";
constexpr const char* Discarded="/.config-migration-discard";
constexpr const char* Orphan="/.config-migration-orphan";
#if MBEDTLS_VERSION_MAJOR < 3
#define migrationShaStart mbedtls_sha256_starts_ret
#define migrationShaUpdate mbedtls_sha256_update_ret
#define migrationShaFinish mbedtls_sha256_finish_ret
#else
#define migrationShaStart mbedtls_sha256_starts
#define migrationShaUpdate mbedtls_sha256_update
#define migrationShaFinish mbedtls_sha256_finish
#endif
bool reject(String& error,const char* reason){error=reason;return false;}
String activePath(uint32_t id,bool backup){
    const char* name=fileName(id);if(!name)return {};
    char path[72];const int length=snprintf(path,sizeof(path),"/config/%s%s",name,backup?".bak":"");
    String result(path);return length>0&&static_cast<size_t>(length)<sizeof(path)&&
        result.length()==static_cast<size_t>(length)?result:String();
}
const char* marker(Phase phase){
    switch(phase){
        case Phase::Applying:return "/.config-migration/applying";
        case Phase::Committed:return "/.config-migration/committed";
        case Phase::RolledBack:return "/.config-migration/rolled-back";
        case Phase::Conflict:return "/.config-migration/conflict";
        case Phase::Invalid:return "/.config-migration/invalid";
        default:return nullptr;
    }
}
bool hashBytes(const void* bytes,size_t size,uint8_t* out){
    mbedtls_sha256_context context;mbedtls_sha256_init(&context);
    const bool ok=migrationShaStart(&context,0)==0&&
        migrationShaUpdate(&context,static_cast<const uint8_t*>(bytes),size)==0&&
        migrationShaFinish(&context,out)==0;
    mbedtls_sha256_free(&context);return ok;
}
bool fingerprint(const String& path,Fingerprint& output){
    output={};if(path.isEmpty())return false;
    if(!LittleFS.exists(path))return true;
    File input=LittleFS.open(path,FILE_READ);
    if(!input||input.isDirectory()||input.size()>MaxFileBytes)return false;
    output.exists=1;output.size=static_cast<uint32_t>(input.size());
    uint8_t buffer[512];uint32_t read=0;
    mbedtls_sha256_context context;mbedtls_sha256_init(&context);
    bool ok=migrationShaStart(&context,0)==0;
    while(ok&&read<output.size){
        const size_t count=input.read(buffer,std::min(sizeof(buffer),static_cast<size_t>(output.size-read)));
        if(!count){ok=false;break;}
        read+=count;ok=migrationShaUpdate(&context,buffer,count)==0;
    }
    ok=ok&&input.size()==output.size&&!input.available()&&migrationShaFinish(&context,output.sha256)==0;
    mbedtls_sha256_free(&context);input.close();return ok;
}
bool matches(const String& path,const Fingerprint& expected){Fingerprint actual;return fingerprint(path,actual)&&same(actual,expected);}
bool copyVerified(const String& from,const String& to,const Fingerprint& expected){
    if(from.isEmpty()||to.isEmpty()||!expected.exists||!matches(from,expected))return false;
    File input=LittleFS.open(from,FILE_READ),output=LittleFS.open(to,FILE_WRITE);
    if(!input||!output)return false;
    uint8_t buffer[512];uint32_t copied=0;bool ok=true;
    while(copied<expected.size){
        const size_t count=input.read(buffer,std::min(sizeof(buffer),static_cast<size_t>(expected.size-copied)));
        if(!count||output.write(buffer,count)!=count){ok=false;break;}copied+=count;
    }
    output.flush();output.close();input.close();
    return ok&&matches(from,expected)&&matches(to,expected);
}
struct DiskManifest {Manifest manifest;uint8_t checksum[32];};
bool readRecord(const char* path,Manifest& output){
    File input=LittleFS.open(path,FILE_READ);if(!input||input.size()!=sizeof(DiskManifest))return false;
    DiskManifest record{};uint8_t hash[32];
    const bool ok=input.read(reinterpret_cast<uint8_t*>(&record),sizeof(record))==sizeof(record)&&
        hashBytes(&record.manifest,sizeof(Manifest),hash)&&!memcmp(hash,record.checksum,32);
    input.close();if(ok)output=record.manifest;return ok;
}
bool writeRecord(const char* path,const Manifest& manifest){
    if(LittleFS.exists(path))return false;
    DiskManifest record{};record.manifest=manifest;
    if(!hashBytes(&record.manifest,sizeof(Manifest),record.checksum))return false;
    // An immutable record is published by rename, never by observing file size.
    const String temporary=String(path)+".tmp";
    if(temporary.length()!=strlen(path)+4)return false;
    File out=LittleFS.open(temporary,FILE_WRITE);if(!out)return false;
    const bool written=out.write(reinterpret_cast<const uint8_t*>(&record),sizeof(record))==sizeof(record);
    out.flush();out.close();Manifest check;
    if(!written||!readRecord(temporary.c_str(),check)||memcmp(&check,&manifest,sizeof(manifest)))return false;
    return LittleFS.rename(temporary,path);
}
bool markerValid(const char* path){
    File file=LittleFS.open(path,FILE_READ);if(!file||file.size()!=12)return false;
    char value[12];return file.read(reinterpret_cast<uint8_t*>(value),sizeof(value))==sizeof(value)&&
        !memcmp(value,"FB-MIGRATE-1",12);
}
}
String Storage::slotPath(size_t index,Slot slot){
    if(index>=MaxFiles)return {};
    char path[64];const int n=snprintf(path,sizeof(path),"%s/%u-%u.bin",Root,
        static_cast<unsigned>(index),static_cast<unsigned>(slot));
    String result(path);return n>0&&static_cast<size_t>(n)<sizeof(path)&&result.length()==static_cast<size_t>(n)?result:String();
}
bool Storage::protectedPath(const String& path){
    return path==Root||path.startsWith("/.config-migration/")||
        path==Discarded||path.startsWith("/.config-migration-discard/")||
        path==Orphan||path.startsWith("/.config-migration-orphan/");
}
bool Storage::orphanRetained()const{return LittleFS.exists(Orphan);}
bool Storage::orphanRevision(String& revision,String& error)const{
    revision="";
    if(!LittleFS.exists(Root)||LittleFS.exists(Ready)||LittleFS.exists(Discarded)||orphanRetained())
        return reject(error,"MIGRATION_ORPHAN_NOT_AVAILABLE");
    for(Phase p:{Phase::Applying,Phase::Committed,Phase::RolledBack,Phase::Conflict,Phase::Invalid})
        if(LittleFS.exists(marker(p)))return reject(error,"MIGRATION_RECOVERY_REQUIRED");
    Manifest manifest;
    if(readReceiving(manifest)&&valid(manifest))return reject(error,"MIGRATION_SESSION_DISCARD_REQUIRED");
    // All possible files created before ready publication have fixed names.
    // Unknown content is left untouched for explicit maintenance, not removed.
    constexpr size_t Count=MaxFiles*3+3;
    Fingerprint records[Count]{};bool seen[Count]{};
    File directory=LittleFS.open(Root,FILE_READ);
    if(!directory||!directory.isDirectory())return reject(error,"MIGRATION_ORPHAN_DIRECTORY_INVALID");
    for(File child=directory.openNextFile();child;child=directory.openNextFile()){
        const char* full=child.name();const char* name=full?strrchr(full,'/'):nullptr;
        name=name?name+1:full;
        if(!name||child.isDirectory())return reject(error,"MIGRATION_ORPHAN_UNKNOWN_FILE");
        size_t found=Count;
        for(size_t index=0;index<Count;++index){
            char expected[40];
            if(index<MaxFiles*3)snprintf(expected,sizeof(expected),"%u-%u.bin",unsigned(index/3),unsigned(index%3));
            else snprintf(expected,sizeof(expected),"%s",index==MaxFiles*3?"receiving.bin":index==MaxFiles*3+1?"receiving.bin.tmp":"ready.bin.tmp");
            if(!strcmp(name,expected)){found=index;break;}
        }
        if(found==Count||seen[found])return reject(error,"MIGRATION_ORPHAN_UNKNOWN_FILE");
        seen[found]=true;
        const String path=String(Root)+"/"+name;
        if(path.length()!=strlen(Root)+1+strlen(name))return reject(error,"MIGRATION_MEMORY_PRESSURE");
        child.close();
        if(!fingerprint(path,records[found])||!records[found].exists)return reject(error,"MIGRATION_ORPHAN_UNREADABLE");
    }
    uint8_t digest[32];if(!hashBytes(records,sizeof(records),digest))return reject(error,"MIGRATION_ORPHAN_UNREADABLE");
    char value[65];for(size_t i=0;i<32;++i)snprintf(value+i*2,3,"%02x",digest[i]);
    revision=value;
    return revision.length()==64||reject(error,"MIGRATION_MEMORY_PRESSURE");
}
bool Storage::quarantineOrphan(const char* reviewedRevision,String& error){
    if(!reviewedRevision||strlen(reviewedRevision)!=64)return reject(error,"MIGRATION_ORPHAN_REVIEW_REQUIRED");
    String current;
    if(!orphanRevision(current,error))return false;
    if(current!=reviewedRevision)return reject(error,"MIGRATION_ORPHAN_CHANGED");
    return LittleFS.rename(Root,Orphan)||reject(error,"MIGRATION_ORPHAN_PRESERVE_FAILED");
}
bool Storage::occupied()const{return LittleFS.exists(Root)||LittleFS.exists(Discarded);}
String Storage::revision(const Manifest& manifest){
    uint8_t digest[32];if(!valid(manifest)||!hashBytes(&manifest,sizeof(manifest),digest))return {};
    char text[65];for(size_t i=0;i<32;++i)snprintf(text+i*2,3,"%02x",digest[i]);
    String result(text);return result.length()==64?result:String();
}
Phase Storage::phase() const {
    // Terminal markers are mutually exclusive. A torn committed marker may be
    // from a prior successful run, so do not roll back subsequent user changes.
    Phase terminal=Phase::None;
    for(Phase p:{Phase::Committed,Phase::RolledBack,Phase::Conflict,Phase::Invalid}){
        if(!LittleFS.exists(marker(p)))continue;
        if(terminal!=Phase::None||!markerValid(marker(p)))return Phase::Blocked;
        terminal=p;
    }
    if(terminal!=Phase::None){Manifest manifest;return readManifest(manifest)&&valid(manifest)?terminal:Phase::Blocked;}
    // Even a torn applying marker requires restoration from the complete backup.
    if(LittleFS.exists(marker(Phase::Applying)))return Phase::Applying;
    return LittleFS.exists(Ready)?Phase::Prepared:Phase::None;
}
bool Storage::readManifest(Manifest& manifest)const{return readRecord(Ready,manifest);}
bool Storage::readReceiving(Manifest& manifest)const{return readRecord(Receiving,manifest);}
bool Storage::readSession(Manifest& manifest)const{
    return readManifest(manifest)||readReceiving(manifest)||
        readRecord("/.config-migration-discard/ready.bin",manifest)||
        readRecord("/.config-migration-discard/receiving.bin",manifest);
}
bool Storage::firmwareMatches(const char* expected)const{return ESP.getSketchMD5()==expected;}
bool Storage::verifySlot(size_t index,Slot slot,const Fingerprint& expected)const{return matches(slotPath(index,slot),expected);}
bool Storage::currentMatches(uint32_t id,bool backup,const Fingerprint& expected)const{return matches(activePath(id,backup),expected);}
bool Storage::install(size_t index,Slot slot,uint32_t id,bool backup,const Fingerprint& expected){
    const String target=activePath(id,backup);if(target.isEmpty())return false;
    if(!expected.exists)return !LittleFS.exists(target)||LittleFS.remove(target);
    const String temporary=target+".migration.tmp";
    if(temporary.length()!=target.length()+14)return false;
    if(!copyVerified(slotPath(index,slot),temporary,expected))return false;
    // LittleFS rename replaces a file atomically; never remove the target first.
    return LittleFS.rename(temporary,target)&&matches(target,expected);
}
bool Storage::mark(Phase value){
    const char* path=marker(value);if(!path)return false;
    if(LittleFS.exists(path))return markerValid(path);
    File output=LittleFS.open(path,FILE_WRITE);if(!output)return false;
    const bool ok=output.write(reinterpret_cast<const uint8_t*>("FB-MIGRATE-1"),12)==12;
    output.flush();output.close();return ok&&markerValid(path);
}
bool Storage::begin(const char* session,const Selection* selected,size_t count,String& error){
    if(!tokenValid(session)||!selected||!count||count>WritableFiles)return reject(error,"MIGRATION_SELECTION_INVALID");
    if(occupied())return reject(error,"MIGRATION_SLOT_OCCUPIED");
    Manifest manifest;manifest.count=MaxFiles;std::strcpy(manifest.session,session);
    const String firmware=ESP.getSketchMD5();
    if(!tokenValid(firmware.c_str()))return reject(error,"MIGRATION_FIRMWARE_UNKNOWN");
    std::strcpy(manifest.firmware,firmware.c_str());
    for(size_t i=0;i<MaxFiles;++i)manifest.entries[i].fileId=i;
    for(size_t i=0;i<count;++i){
        if(selected[i].fileId>=WritableFiles||!selected[i].size||selected[i].size>MaxFileBytes||
            manifest.entries[selected[i].fileId].candidate.exists)return reject(error,"MIGRATION_SELECTION_INVALID");
        auto& candidate=manifest.entries[selected[i].fileId].candidate;candidate.exists=1;candidate.size=selected[i].size;
    }
    if(!valid(manifest))return reject(error,"MIGRATION_SELECTION_INVALID");
    uint64_t required=8192;
    for(size_t i=0;i<MaxFiles;++i){
        auto& row=manifest.entries[i];
        if(!fingerprint(activePath(row.fileId,false),row.original)||!fingerprint(activePath(row.fileId,true),row.backup))
            return reject(error,"MIGRATION_BASELINE_UNAVAILABLE");
        required+=row.original.size+row.backup.size+uint64_t(row.candidate.size)*3;
    }
    const size_t total=LittleFS.totalBytes(),used=LittleFS.usedBytes();
    if(total<used||required>total-used)return reject(error,"MIGRATION_STORAGE_CAPACITY");
    if(!LittleFS.mkdir(Root))return reject(error,"MIGRATION_STAGE_CREATE_FAILED");
    if(!writeRecord(Receiving,manifest))return reject(error,"MIGRATION_SESSION_SAVE_FAILED");
    for(size_t i=0;i<MaxFiles;++i){
        const auto& row=manifest.entries[i];
        if((row.original.exists&&!copyVerified(activePath(row.fileId,false),slotPath(i,Slot::Original),row.original))||
            (row.backup.exists&&!copyVerified(activePath(row.fileId,true),slotPath(i,Slot::Backup),row.backup)))
            return reject(error,"MIGRATION_BASELINE_COPY_FAILED");
    }
    return true;
}
bool Storage::matchesOriginalRange(const char* session,uint32_t id,uint32_t offset,uint32_t total,
        const uint8_t* bytes,size_t size,String& error) const {
    Manifest manifest;
    if((id!=3&&id!=4)||phase()!=Phase::None||!readReceiving(manifest)||!valid(manifest)||
       !tokenValid(session)||strcmp(session,manifest.session)||total>MaxFileBytes||size>4096||
       offset>total||size>total-offset||(!bytes&&size))return reject(error,"MIGRATION_BASELINE_REQUEST_INVALID");
    for(size_t index=0;index<manifest.count;++index){
        const auto& row=manifest.entries[index];if(row.fileId!=id)continue;
        const auto& original=row.original.exists?row.original:row.backup;
        if(!original.exists)return (total==0&&offset==0&&size==0)||reject(error,"MIGRATION_BASELINE_CHANGED");
        if(!total||!size||total!=original.size)return reject(error,"MIGRATION_BASELINE_CHANGED");
        const Slot slot=row.original.exists?Slot::Original:Slot::Backup;
        File input=LittleFS.open(slotPath(index,slot),FILE_READ);
        if(!input||input.size()!=total||!input.seek(offset))return reject(error,"MIGRATION_BASELINE_UNAVAILABLE");
        uint8_t buffer[256];size_t compared=0;
        while(compared<size){
            const size_t count=std::min(sizeof(buffer),size-compared);
            if(input.read(buffer,count)!=count||memcmp(buffer,bytes+compared,count))return reject(error,"MIGRATION_BASELINE_CHANGED");
            compared+=count;
        }
        return true; // inspect/prepare also verify retained hashes and live generations.
    }
    return reject(error,"MIGRATION_BASELINE_UNAVAILABLE");
}
bool Storage::append(const char* session,uint32_t id,uint32_t offset,const uint8_t* bytes,size_t size,String& error){
    Manifest manifest;
    if(phase()!=Phase::None||!readReceiving(manifest)||!valid(manifest)||!tokenValid(session)||
        strcmp(manifest.session,session)||id>=WritableFiles||!bytes||!size||size>4096)
        return reject(error,"MIGRATION_SESSION_INVALID");
    const auto& candidate=manifest.entries[id].candidate;
    if(!candidate.exists||offset>candidate.size||size>candidate.size-offset)return reject(error,"MIGRATION_RANGE_INVALID");
    const String path=slotPath(id,Slot::Candidate);
    File existing=LittleFS.open(path,FILE_READ);const size_t current=existing?existing.size():0;
    if(offset<current&&offset+size==current){
        if(!existing.seek(offset))return reject(error,"MIGRATION_REPLAY_READ_FAILED");
        for(size_t i=0;i<size;++i)if(existing.read()!=bytes[i])return reject(error,"MIGRATION_REPLAY_MISMATCH");
        return true;
    }
    existing.close();if(offset!=current)return reject(error,"MIGRATION_RANGE_INVALID");
    File output=LittleFS.open(path,FILE_APPEND);if(!output)return reject(error,"MIGRATION_STAGE_WRITE_FAILED");
    const size_t written=output.write(bytes,size);output.flush();const size_t stored=output.size();output.close();
    return (written==size&&stored==offset+size)||reject(error,"MIGRATION_STAGE_WRITE_FAILED");
}
bool Storage::inspect(const char* session,Manifest& manifest,String& error) const{
    if(phase()!=Phase::None||!readReceiving(manifest)||!valid(manifest)||!tokenValid(session)||
        strcmp(manifest.session,session)||!firmwareMatches(manifest.firmware))
        return reject(error,"MIGRATION_SESSION_INVALID");
    for(size_t i=0;i<MaxFiles;++i){
        auto& row=manifest.entries[i];
        if(!currentMatches(row.fileId,false,row.original)||!currentMatches(row.fileId,true,row.backup))
            return reject(error,"MIGRATION_BASELINE_CHANGED");
        if(!verifySlot(i,Slot::Original,row.original)||!verifySlot(i,Slot::Backup,row.backup))
            return reject(error,"MIGRATION_BASELINE_COPY_FAILED");
        if(row.candidate.exists){
            Fingerprint value;if(!fingerprint(slotPath(i,Slot::Candidate),value)||!value.exists||value.size!=row.candidate.size)
                return reject(error,"MIGRATION_INCOMPLETE");
            row.candidate=value;
        }
    }
    return true;
}
bool Storage::prepare(const char* session,const char* reviewedRevision,Validator validate,void* context,String& error){
    Manifest manifest;
    if(!inspect(session,manifest,error))return false;
    const String currentRevision=revision(manifest);
    if(!reviewedRevision||strlen(reviewedRevision)!=64||currentRevision.length()!=64||
        currentRevision!=reviewedRevision)return reject(error,"MIGRATION_PREVIEW_CHANGED");
    if(!validate||!validate(manifest,context,error))return false;
    // Recheck after potentially expensive graph/schema preflight; validation
    // never grants an active write or rebases on a concurrently changed graph.
    for(size_t i=0;i<MaxFiles;++i){const auto& row=manifest.entries[i];
        if(!currentMatches(row.fileId,false,row.original)||!currentMatches(row.fileId,true,row.backup)||
            (row.candidate.exists&&!verifySlot(i,Slot::Candidate,row.candidate)))return reject(error,"MIGRATION_BASELINE_CHANGED");
    }
    return writeRecord(Ready,manifest)||reject(error,"MIGRATION_PREPARE_FAILED");
}
bool Storage::discard(const char* session,String& error){
    // Only explicit, session-bound disposal is allowed. Rename the complete
    // directory first so a power cut during cleanup cannot leave an applying
    // journal with its rollback generations missing.
    if(LittleFS.exists(Root)){
        const Phase state=phase();
        if(state==Phase::Applying||state==Phase::Blocked||LittleFS.exists(Discarded))
            return reject(error,"MIGRATION_RECOVERY_REQUIRED");
        Manifest manifest;
        if(!(readManifest(manifest)||readReceiving(manifest))||!valid(manifest)||
            !tokenValid(session)||strcmp(session,manifest.session))
            return reject(error,"MIGRATION_SESSION_INVALID");
        if(!LittleFS.rename(Root,Discarded))return reject(error,"MIGRATION_DISCARD_FAILED");
    }
    // Cleanup can be resumed after power loss, but still requires the session
    // retained in the renamed manifest. Delete it last.
    if(!tokenValid(session))return reject(error,"MIGRATION_SESSION_INVALID");
    { File directory=LittleFS.open(Discarded,FILE_READ);
      if(directory&&directory.isDirectory()){
          File child=directory.openNextFile();
          if(!child){directory.close();return LittleFS.rmdir(Discarded)||reject(error,"MIGRATION_DISCARD_FAILED");}
      }
    }
    Manifest manifest;
    if(!(readRecord("/.config-migration-discard/ready.bin",manifest)||
         readRecord("/.config-migration-discard/receiving.bin",manifest))||
       !valid(manifest)||!tokenValid(session)||strcmp(session,manifest.session))
        return reject(error,"MIGRATION_SESSION_INVALID");
    for(size_t i=0;i<MaxFiles;++i)for(unsigned slot=0;slot<3;++slot){
        char path[80];snprintf(path,sizeof(path),"%s/%u-%u.bin",Discarded,unsigned(i),slot);
        if(LittleFS.exists(path)&&!LittleFS.remove(path))return reject(error,"MIGRATION_DISCARD_FAILED");
    }
    for(const char* name:{"applying","committed","rolled-back","conflict","invalid","ready.bin.tmp","receiving.bin.tmp","ready.bin","receiving.bin"}){
        const String path=String(Discarded)+"/"+name;
        if(path.length()!=strlen(Discarded)+1+strlen(name))return reject(error,"MIGRATION_MEMORY_PRESSURE");
        if(LittleFS.exists(path)&&!LittleFS.remove(path))return reject(error,"MIGRATION_DISCARD_FAILED");
    }
    return LittleFS.rmdir(Discarded)||reject(error,"MIGRATION_DISCARD_FAILED");
}
bool recoverAtBoot(){
    Guard guard;if(!guard)return false;
    Storage storage;return recoverBeforeLoad(storage)!=Phase::Blocked;
}
} }
