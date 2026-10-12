#include "systems/ConfigMigrationJournal.h"
#include <array>
#include <cassert>
#include <stdexcept>
#include <string>
using namespace FastBee::ConfigMigration;
struct PowerLoss {};
struct Store {
    Manifest manifest;
    Phase state=Phase::Prepared;
    std::array<Fingerprint,MaxFiles*2> active;
    std::array<std::array<Fingerprint,3>,MaxFiles> slots;
    bool firmware=true,manifestReadable=true;
    int powerAfter=-1,failInstall=-1,installAttempts=0;
    unsigned writes=0;
    Phase failMark=Phase::None;
    void changed(){++writes;if(powerAfter==0)throw PowerLoss{};if(powerAfter>0)--powerAfter;}
    Phase phase(){return state;}
    bool readManifest(Manifest& out){out=manifest;return manifestReadable;}
    bool firmwareMatches(const char*){return firmware;}
    bool verifySlot(size_t i,Slot slot,const Fingerprint& value){return same(slots[i][static_cast<size_t>(slot)],value);}
    bool currentMatches(uint32_t id,bool backup,const Fingerprint& value){return same(active[id*2+backup],value);}
    bool install(size_t i,Slot slot,uint32_t id,bool backup,const Fingerprint& value){
        if(installAttempts++==failInstall)return false;
        if(!verifySlot(i,slot,value))return false;
        active[id*2+backup]=value;changed();return true;
    }
    bool mark(Phase next){if(next==failMark)return false;state=next;changed();return true;}
};
static Fingerprint fingerprint(unsigned value){Fingerprint result;result.exists=1;result.size=10;
    result.sha256[0]=static_cast<uint8_t>(value);return result;}
static Store fixture(){
    Store s;s.manifest.count=MaxFiles;
    std::strcpy(s.manifest.session,"0123456789abcdef0123456789abcdef");
    std::strcpy(s.manifest.firmware,"abcdef0123456789abcdef0123456789");
    for(size_t i=0;i<MaxFiles;++i){
        auto& row=s.manifest.entries[i];row.fileId=i;
        row.original=i==3?Fingerprint{}:fingerprint(i+1);row.backup=i==3?Fingerprint{}:fingerprint(i+20);
        if(i==2||i==3||i==5)row.candidate=fingerprint(i+60);
        s.slots[i]={{row.candidate,row.original,row.backup}};
        s.active[i*2]=row.original;s.active[i*2+1]=row.backup;
    }
    assert(valid(s.manifest));return s;
}
static void verify(const Store& s,bool committed){
    for(size_t i=0;i<MaxFiles;++i){const auto& row=s.manifest.entries[i];
        const bool replaced=committed&&row.candidate.exists;
        assert(same(s.active[i*2],replaced?row.candidate:row.original));
        assert(same(s.active[i*2+1],replaced?row.candidate:row.backup));
    }
}
int main(int argc,char**argv){
    assert(argc==2);const std::string mode=argv[1];
    if(mode=="power-loss"){
        for(int cut=0;cut<8;++cut){auto s=fixture();s.powerAfter=cut;
            try{recoverBeforeLoad(s);}catch(const PowerLoss&){}
            s.powerAfter=-1;const auto result=recoverBeforeLoad(s);
            assert(result==Phase::Committed||result==Phase::RolledBack);verify(s,result==Phase::Committed);
            const unsigned writes=s.writes;assert(recoverBeforeLoad(s)==result&&s.writes==writes);
        }
    }else if(mode=="recovery-power-loss"){
        for(int cut=0;cut<7;++cut){auto s=fixture();s.powerAfter=3;
            try{recoverBeforeLoad(s);}catch(const PowerLoss&){}
            assert(s.state==Phase::Applying);s.powerAfter=cut;
            try{recoverBeforeLoad(s);}catch(const PowerLoss&){}
            s.powerAfter=-1;assert(recoverBeforeLoad(s)==Phase::RolledBack);verify(s,false);
        }
    }else if(mode=="write-failure"){
        for(int fail=0;fail<6;++fail){auto s=fixture();s.failInstall=fail;
            assert(recoverBeforeLoad(s)==Phase::RolledBack);verify(s,false);}
    }else if(mode=="failed-recovery"){
        auto s=fixture();s.state=Phase::Applying;s.active[4]=fingerprint(240);s.failInstall=0;
        assert(recoverBeforeLoad(s)==Phase::Blocked);s.failInstall=-1;
        assert(recoverBeforeLoad(s)==Phase::RolledBack);verify(s,false);
    }else if(mode=="candidate-corrupt"){
        auto s=fixture();s.slots[3][0].sha256[0]^=1;
        assert(recoverBeforeLoad(s)==Phase::Invalid&&s.installAttempts==0);verify(s,false);
    }else if(mode=="backup-corrupt"){
        auto s=fixture();s.state=Phase::Applying;s.slots[2][2].sha256[0]^=1;
        assert(recoverBeforeLoad(s)==Phase::Blocked&&s.writes==0);verify(s,false);
    }else if(mode=="dependency-conflict"){
        for(size_t i=0;i<MaxFiles*2;++i){auto s=fixture();s.active[i]=fingerprint(250);
            const auto before=s.active;assert(recoverBeforeLoad(s)==Phase::Conflict&&s.installAttempts==0);
            for(size_t n=0;n<before.size();++n)assert(same(before[n],s.active[n]));}
    }else if(mode=="firmware-conflict"){
        auto s=fixture();s.firmware=false;assert(recoverBeforeLoad(s)==Phase::Conflict);verify(s,false);
    }else if(mode=="markers"){
        auto s=fixture();s.failMark=Phase::Applying;assert(recoverBeforeLoad(s)==Phase::Blocked);verify(s,false);
        s=fixture();s.failMark=Phase::Committed;assert(recoverBeforeLoad(s)==Phase::Blocked);verify(s,true);
        s.failMark=Phase::None;assert(recoverBeforeLoad(s)==Phase::RolledBack);verify(s,false);
    }else if(mode=="manifest"){
        auto s=fixture();s.manifest.entries[0].fileId=99;assert(recoverBeforeLoad(s)==Phase::Blocked&&s.writes==0);
        s=fixture();s.manifest.entries[8].candidate=fingerprint(200);assert(!valid(s.manifest));
        s=fixture();s.manifest.count=0;assert(!valid(s.manifest));
        s=fixture();s.manifest.entries[0].fileId=1;assert(!valid(s.manifest));
        s=fixture();s.manifestReadable=false;assert(recoverBeforeLoad(s)==Phase::Blocked&&s.writes==0);
    }else if(mode=="committed-user-edits"){
        auto s=fixture();assert(recoverBeforeLoad(s)==Phase::Committed);s.active[4]=fingerprint(233);
        const unsigned writes=s.writes;assert(recoverBeforeLoad(s)==Phase::Committed&&s.writes==writes);
        assert(s.active[4].sha256[0]==233);
    }else assert(false);
}
