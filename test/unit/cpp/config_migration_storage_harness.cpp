#include "systems/ConfigMigrationStorage.h"
#include <LittleFS.h>
#include <mbedtls/sha256.h>
#include <cassert>
#include <string>
using namespace FastBee::ConfigMigration;
constexpr const char* Session="0123456789abcdef0123456789abcdef";
static std::string active(unsigned id,bool backup=false){return std::string("/config/")+fileName(id)+(backup?".bak":"");}
static void fixture(){
    LittleFS={};migrationHashFails=false;ESP.firmware="abcdef0123456789abcdef0123456789";
    LittleFS.directories.insert("/config");
    for(unsigned i=0;i<MaxFiles;++i){
        LittleFS.files[active(i)]="original-"+std::to_string(i);
        LittleFS.files[active(i,true)]="backup-"+std::to_string(i);
    }
    LittleFS.files.erase(active(3));LittleFS.files.erase(active(3,true));
}
static void verifyOld(){
    for(unsigned i=0;i<MaxFiles;++i)for(bool backup:{false,true}){
        if(i==3)assert(!LittleFS.files.count(active(i,backup)));
        else assert(LittleFS.files.at(active(i,backup))==std::string(backup?"backup-":"original-")+std::to_string(i));
    }
}
static void upload(Storage& storage){
    String error;Selection files[]={{2,3},{3,3}};
    assert(storage.begin(Session,files,2,error));
    assert(storage.append(Session,2,0,reinterpret_cast<const uint8_t*>("new"),3,error));
    assert(storage.append(Session,3,0,reinterpret_cast<const uint8_t*>("add"),3,error));
}
static bool approve(const Manifest&,void*,String&){return true;}
static void ready(Storage& storage){
    upload(storage);String error;Manifest manifest;
    assert(storage.inspect(Session,manifest,error));
    const String revision=Storage::revision(manifest);assert(revision.length()==64);
    assert(storage.prepare(Session,revision.c_str(),approve,nullptr,error));
    assert(storage.phase()==Phase::Prepared);
}
int main(int argc,char** argv){
    assert(argc==2);const std::string mode=argv[1];fixture();Storage storage;String error;
    if(mode=="staging"){
        upload(storage);verifyOld();
        assert(storage.append(Session,2,0,reinterpret_cast<const uint8_t*>("new"),3,error));
        assert(!storage.append(Session,2,0,reinterpret_cast<const uint8_t*>("bad"),3,error));
        assert(!storage.append(Session,2,1,reinterpret_cast<const uint8_t*>("x"),1,error));
        assert(!storage.append("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",2,0,reinterpret_cast<const uint8_t*>("new"),3,error));
        Selection selection{2,3};assert(!storage.begin(Session,&selection,1,error));verifyOld();
    }else if(mode=="original-range"){
        upload(storage);const unsigned mutations=LittleFS.mutations;
        const auto bytes=[](const char* text){return reinterpret_cast<const uint8_t*>(text);};
        assert(storage.matchesOriginalRange(Session,4,0,10,bytes("original-4"),10,error));
        assert(storage.matchesOriginalRange(Session,4,8,10,bytes("-4"),2,error));
        assert(storage.matchesOriginalRange(Session,3,0,0,nullptr,0,error));
        assert(!storage.matchesOriginalRange(Session,3,0,1,bytes("x"),1,error));
        assert(!storage.matchesOriginalRange(Session,4,0,10,bytes("original-X"),10,error));
        assert(!storage.matchesOriginalRange(Session,4,9,10,bytes("-4"),2,error));
        assert(!storage.matchesOriginalRange(Session,4,0,9,bytes("original-"),9,error));
        assert(!storage.matchesOriginalRange("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",4,0,10,bytes("original-4"),10,error));
        assert(!storage.matchesOriginalRange(Session,8,0,10,bytes("original-8"),10,error));
        assert(!storage.matchesOriginalRange(nullptr,4,0,10,bytes("original-4"),10,error));
        assert(LittleFS.mutations==mutations);
        LittleFS.files[active(4)]="concurrent";
        assert(storage.matchesOriginalRange(Session,4,0,10,bytes("original-4"),10,error));
        Manifest manifest;assert(!storage.inspect(Session,manifest,error)); // Live edit prevents preparation.
    }else if(mode=="original-backup"){
        LittleFS.files.erase(active(4));upload(storage);
        assert(storage.matchesOriginalRange(Session,4,0,8,reinterpret_cast<const uint8_t*>("backup-4"),8,error));
        assert(!storage.matchesOriginalRange(Session,4,0,0,nullptr,0,error));
    }else if(mode=="review"){
        upload(storage);Manifest manifest;assert(storage.inspect(Session,manifest,error));
        const String revision=Storage::revision(manifest);
        assert(!storage.prepare(Session,std::string(64,'0').c_str(),approve,nullptr,error));
        assert(!storage.prepare(Session,revision.c_str(),[](const Manifest&,void*,String&){return false;},nullptr,error));
        assert(!storage.prepare(Session,revision.c_str(),[](const Manifest&,void*,String&){LittleFS.files[active(9)]="concurrent";return true;},nullptr,error));
        assert(storage.phase()==Phase::None);assert(LittleFS.files[active(9)]=="concurrent");
    }else if(mode=="commit"){
        ready(storage);verifyOld();assert(recoverAtBoot());assert(storage.phase()==Phase::Committed);
        for(bool backup:{false,true}){assert(LittleFS.files[active(2,backup)]=="new");assert(LittleFS.files[active(3,backup)]=="add");}
        LittleFS.files[active(2)]="later-user-edit";assert(recoverAtBoot());assert(LittleFS.files[active(2)]=="later-user-edit");
    }else if(mode=="conflict"){
        ready(storage);LittleFS.files[active(8,true)]="changed";assert(recoverAtBoot());assert(storage.phase()==Phase::Conflict);
        assert(LittleFS.files[active(2)]=="original-2");
    }else if(mode=="corrupt"){
        ready(storage);LittleFS.files["/.config-migration/2-0.bin"]="bad";
        assert(recoverAtBoot());assert(storage.phase()==Phase::Invalid);verifyOld();
    }else if(mode=="capacity-hash-short-write"){
        Selection selected{2,3};LittleFS.capacity=LittleFS.usedBytes()+50;
        assert(!storage.begin(Session,&selected,1,error));assert(!storage.occupied());
        fixture();migrationHashFails=true;assert(!storage.begin(Session,&selected,1,error));verifyOld();
        fixture();LittleFS.writeLimit=1;assert(!storage.begin(Session,&selected,1,error));verifyOld();
    }else if(mode=="install-failure"){
        ready(storage);LittleFS.failRenameTo=active(2);assert(!recoverAtBoot());
        LittleFS.failRenameTo="";assert(recoverAtBoot());assert(storage.phase()==Phase::RolledBack);verifyOld();
    }else if(mode=="power-loss"){
        ready(storage);const auto saved=LittleFS;unsigned cases=0;
        for(int cut=0;cut<100;++cut){LittleFS=saved;LittleFS.powerAfter=cut;bool lost=false;
            try{recoverAtBoot();}catch(const MigrationPowerLoss&){lost=true;}
            LittleFS.powerAfter=-1;const bool recovered=recoverAtBoot();++cases;
            if(!recovered){assert(storage.phase()==Phase::Blocked);}
            else if(storage.phase()==Phase::RolledBack)verifyOld();
            else {assert(storage.phase()==Phase::Committed);assert(LittleFS.files[active(2)]=="new");assert(LittleFS.files[active(3)]=="add");}
            if(!lost)break;
        }assert(cases>12);
    }else if(mode=="discard-power-loss"){
        ready(storage);const auto saved=LittleFS;unsigned cases=0;
        for(int cut=0;cut<100;++cut){LittleFS=saved;LittleFS.powerAfter=cut;bool lost=false;
            try{assert(storage.discard(Session,error));}catch(const MigrationPowerLoss&){lost=true;}
            LittleFS.powerAfter=-1;if(storage.occupied())assert(storage.discard(Session,error));
            assert(!storage.occupied());verifyOld();++cases;if(!lost)break;
        }assert(cases>20);
    }else if(mode=="discard-guards"){
        ready(storage);const unsigned before=LittleFS.mutations;
        assert(!storage.discard("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",error));assert(LittleFS.mutations==before);
        assert(storage.mark(Phase::Applying));assert(!storage.discard(Session,error));
        assert(recoverAtBoot());assert(storage.discard(Session,error));assert(!storage.occupied());verifyOld();
    }else if(mode=="orphan-preservation"){
        LittleFS.directories.insert("/.config-migration");
        LittleFS.files["/.config-migration/receiving.bin.tmp"]="torn-record";
        LittleFS.files["/.config-migration/2-1.bin"]="retained-bytes";
        const unsigned before=LittleFS.mutations;String revision;
        assert(storage.orphanRevision(revision,error));assert(revision.length()==64);assert(LittleFS.mutations==before);
        assert(storage.quarantineOrphan(revision.c_str(),error));assert(!storage.occupied());assert(storage.orphanRetained());
        assert(LittleFS.files["/.config-migration-orphan/receiving.bin.tmp"]=="torn-record");
        assert(LittleFS.files["/.config-migration-orphan/2-1.bin"]=="retained-bytes");verifyOld();
        Selection selected{2,3};assert(storage.begin(Session,&selected,1,error));
        assert(storage.orphanRetained());verifyOld();
    }else if(mode=="orphan-guards"){
        upload(storage);String revision;
        assert(!storage.orphanRevision(revision,error)); // Valid sessions use session-bound discard.
        LittleFS.files["/.config-migration/receiving.bin"]="torn";
        assert(storage.orphanRevision(revision,error));
        LittleFS.files["/.config-migration/receiving.bin"]="different";
        assert(!storage.quarantineOrphan(revision.c_str(),error));
        const auto saved=LittleFS;
        for(const char* name:{"ready.bin","applying","committed","rolled-back","conflict","invalid","unknown"}){
            LittleFS=saved;LittleFS.files[std::string("/.config-migration/")+name]="";
            const unsigned before=LittleFS.mutations;
            assert(!storage.orphanRevision(revision,error));assert(LittleFS.mutations==before);verifyOld();
        }
        LittleFS=saved;LittleFS.directories.insert("/.config-migration-orphan");
        assert(!storage.orphanRevision(revision,error));verifyOld();
    }else if(mode=="orphan-rename-failure"){
        LittleFS.directories.insert("/.config-migration");
        LittleFS.files["/.config-migration/receiving.bin.tmp"]="torn";String revision;
        assert(storage.orphanRevision(revision,error));LittleFS.failRenameTo="/.config-migration-orphan";
        assert(!storage.quarantineOrphan(revision.c_str(),error));assert(storage.occupied());assert(!storage.orphanRetained());
        assert(LittleFS.files["/.config-migration/receiving.bin.tmp"]=="torn");verifyOld();
    }else if(mode=="orphan-power-loss"){
        LittleFS.directories.insert("/.config-migration");
        LittleFS.files["/.config-migration/receiving.bin.tmp"]="torn";String revision;
        assert(storage.orphanRevision(revision,error));LittleFS.powerAfter=0;bool lost=false;
        try{storage.quarantineOrphan(revision.c_str(),error);}catch(const MigrationPowerLoss&){lost=true;}
        assert(lost);LittleFS.powerAfter=-1;assert(recoverAtBoot());assert(!storage.occupied());assert(storage.orphanRetained());
        assert(LittleFS.files["/.config-migration-orphan/receiving.bin.tmp"]=="torn");verifyOld();
    }else if(mode=="protected-paths"){
        assert(Storage::protectedPath("/.config-migration"));assert(Storage::protectedPath("/.config-migration/8-1.bin"));
        assert(Storage::protectedPath("/.config-migration-discard/ready.bin"));assert(!Storage::protectedPath("/config/device.json"));
        assert(Storage::protectedPath("/.config-migration-orphan"));assert(Storage::protectedPath("/.config-migration-orphan/receiving.bin.tmp"));
    }else assert(false);
}
