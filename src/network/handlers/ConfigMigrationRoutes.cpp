#include "network/handlers/SystemRouteHandler.h"
#include "core/FeatureFlags.h"
#if FASTBEE_ENABLE_CONFIG_TRANSFER
#include "network/WebHandlerContext.h"
#include "network/handlers/HandlerUtils.h"
#include "network/NetworkManager.h"
#include "network/NetworkResourceConfig.h"
#include "systems/ConfigMigrationValidation.h"
#include "core/BoardProfile.h"
#include "core/ConfigImportSession.h"
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_system.h>
#include <new>

namespace {
using namespace FastBee::ConfigMigration;
using MigrationStorage = FastBee::ConfigMigration::Storage;
const char* phaseName(Phase phase){
    switch(phase){
        case Phase::None:return "receiving";
        case Phase::Prepared:return "prepared";
        case Phase::Applying:return "applying";
        case Phase::Committed:return "committed";
        case Phase::RolledBack:return "rolled_back";
        case Phase::Conflict:return "conflict";
        case Phase::Invalid:return "invalid";
        default:return "blocked";
    }
}
bool decimal(const String& input,uint32_t limit,uint32_t& output){
    return input.length()==strlen(input.c_str())&&FastBee::ConfigImportSession::decimal(input.c_str(),limit,output);
}
const String& parameter(AsyncWebServerRequest* request,const char* key){
    static const String empty;
    return request->hasParam(key,true)?request->getParam(key,true)->value():empty;
}
void status(MigrationStorage& storage,JsonDocument& output){
    const Phase phase=storage.phase();
    output["phase"]=storage.occupied()?phaseName(phase):"empty";
    output["applied"]=phase==Phase::Committed;
    output["restartRequired"]=phase==Phase::Prepared;
    output["activation"]="next_boot";
    output["orphanRetained"]=storage.orphanRetained();
    Manifest manifest;
    if(!storage.readSession(manifest)||!valid(manifest)){
        if(storage.occupied())output["phase"]="incomplete";
        String revision,error;
        if(storage.orphanRevision(revision,error)){
            output["canQuarantine"]=true;output["orphanRevision"]=revision;
        }
        File directory=LittleFS.open("/.config-migration-discard",FILE_READ);
        if(directory&&directory.isDirectory()&&!directory.openNextFile()){
            // Only an empty, already-discarded directory remains. This token
            // cannot authorize deleting an occupied or active journal.
            output["phase"]="discarding";output["session"]="00000000000000000000000000000000";
        }
        return;
    }
    output["session"]=manifest.session;
    output["firmware"]=manifest.firmware;
    if(LittleFS.exists("/.config-migration-discard"))output["phase"]="discarding";
    if(phase==Phase::Prepared)output["revision"]=MigrationStorage::revision(manifest);
    JsonArray files=output["files"].to<JsonArray>();
    for(size_t i=0;i<manifest.count;++i){const auto& entry=manifest.entries[i];if(!entry.candidate.exists)continue;
        JsonObject row=files.add<JsonObject>();row["name"]=fileName(entry.fileId);row["size"]=entry.candidate.size;
        File file=LittleFS.open(MigrationStorage::slotPath(i,Slot::Candidate),FILE_READ);row["received"]=file?file.size():0;
    }
}
ReviewContext context(WebHandlerContext* web,JsonDocument& report,uint32_t acknowledgements,bool commit){
    ReviewContext result;result.report=&report;result.requireAcknowledgements=commit;
    const auto& profile=FastBee::getCompiledBoardProfile();
    result.resources.wifiActive=WiFi.getMode()!=WIFI_MODE_NULL;
    result.resources.uart0ConsoleActive=true;
    result.resources.usbSerialJtagActive=profile.id==FastBee::BoardProfileId::ESP32_C6_DEVKITC_1||
        profile.id==FastBee::BoardProfileId::ESP32_C3_GENERIC;
    result.resources.acknowledgements=acknowledgements;
    if(web->networkManager){const auto& config=static_cast<FBNetworkManager*>(web->networkManager)->getConfig();
        result.resources.ethernet=FastBee::ethernetResourceConfig(config);
        result.resources.cellular=FastBee::cellularResourceConfig(config);
    }
    return result;
}
bool admitted(WebHandlerContext* ctx,AsyncWebServerRequest* request,bool heavy){
    if(!ctx->requireAuth(request)||!ctx->requireDeveloperMode(request))return false;
    if(heavy&&(HandlerUtils::rejectHeavyRequestOnPressure(request,"Configuration migration",MemoryGuardLevel::SEVERE,8)||
        HandlerUtils::checkLowMemory(request,32768)))return false;
    return true;
}
}

void SystemRouteHandler::setupMigrationRoutes(AsyncWebServer* server){
    server->on("/api/config/migration",HTTP_GET,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,false))return;
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;JsonDocument data;status(storage,data);ctx->sendSuccess(request,data);
    });
    server->on("/api/config/migration/begin",HTTP_POST,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,true))return;
        const String& raw=parameter(request,"files");
        JsonDocument document;
        if(raw.isEmpty()||raw.length()>2048||deserializeJson(document,raw.c_str(), raw.length())||!document.is<JsonArrayConst>()||
           document.size()==0||document.size()>WritableFiles){ctx->sendError(request,400,"MIGRATION_SELECTION_INVALID");return;}
        Selection selected[WritableFiles]{};size_t count=0;
        for(JsonObjectConst row:document.as<JsonArrayConst>()){
            const char* name=row["name"]|"";const int id=fileId(name);
            if(id<0||id>=int(WritableFiles)||!row["name"].is<JsonString>()||
               row["name"].as<JsonString>().size()!=strlen(name)||!row["size"].is<uint32_t>()){
                ctx->sendError(request,400,"MIGRATION_SELECTION_INVALID");return;
            }
            selected[count++]={uint32_t(id),row["size"].as<uint32_t>()};
        }
        uint8_t random[16];esp_fill_random(random,sizeof(random));char token[33];
        for(size_t i=0;i<sizeof(random);++i)snprintf(token+i*2,3,"%02x",random[i]);
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;String error;
        if(!storage.begin(token,selected,count,error)){ctx->sendError(request,409,error.c_str());return;}
        JsonDocument data;status(storage,data);ctx->sendSuccess(request,data);
    });
    server->on("/api/config/migration/check-original",HTTP_POST,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,true))return;
        const String& session=parameter(request,"session");const String& name=parameter(request,"name");
        const String& content=parameter(request,"content");uint32_t offset=0,total=0;
        const int id=fileId(name.c_str());
        if(session.length()!=32||name.length()!=strlen(name.c_str())||(id!=3&&id!=4)||
           !decimal(parameter(request,"offset"),MaxFileBytes,offset)||
           !decimal(parameter(request,"total"),MaxFileBytes,total)||content.length()>4096){
            ctx->sendError(request,400,"MIGRATION_BASELINE_REQUEST_INVALID");return;
        }
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;String error;
        if(!storage.matchesOriginalRange(session.c_str(),uint32_t(id),offset,total,
            reinterpret_cast<const uint8_t*>(content.c_str()),content.length(),error)){
            ctx->sendError(request,409,error.c_str());return;
        }
        JsonDocument data;data["session"]=session;data["name"]=name;data["matched"]=true;
        data["total"]=total;data["checked"]=offset+content.length();ctx->sendSuccess(request,data);
    });
    server->on("/api/config/migration/chunk",HTTP_POST,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,true))return;
        const String& session=parameter(request,"session");const String& name=parameter(request,"name");
        const String& content=parameter(request,"content");uint32_t offset=0;
        const int id=fileId(name.c_str());
        if(session.length()!=32||name.length()!=strlen(name.c_str())||id<0||id>=int(WritableFiles)||
           !decimal(parameter(request,"offset"),MaxFileBytes,offset)||content.isEmpty()||content.length()>4096){
            ctx->sendError(request,400,"MIGRATION_RANGE_INVALID");return;
        }
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;String error;
        if(!storage.append(session.c_str(),uint32_t(id),offset,reinterpret_cast<const uint8_t*>(content.c_str()),content.length(),error)){
            ctx->sendError(request,409,error.c_str());return;
        }
        JsonDocument data;data["session"]=session;data["name"]=name;data["received"]=offset+content.length();ctx->sendSuccess(request,data);
    });
    server->on("/api/config/migration/preview",HTTP_POST,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,true))return;
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;Manifest manifest;String error;JsonDocument data;
        const String& session=parameter(request,"session");
        if(session.length()!=32||!storage.inspect(session.c_str(),manifest,error)){ctx->sendError(request,409,error.c_str());return;}
        auto review=context(ctx,data,0,false);
        const bool validGraph=validateGraph(manifest,&review,error);
        // Validation is a preview even when it rejects a reference. Nothing has
        // been written to the active graph and no automatic commit follows.
        data["valid"]=validGraph;data["error"]=error;data["session"]=session;
        data["revision"]=MigrationStorage::revision(manifest);data["phase"]="reviewed";
        data["activation"]="next_boot";ctx->sendSuccess(request,data);
    });
    server->on("/api/config/migration/prepare",HTTP_POST,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,true))return;
        uint32_t acknowledgements=0;const String& session=parameter(request,"session");
        if(parameter(request,"confirm")!="PREPARE"||session.length()!=32||
           parameter(request,"revision").length()!=64||!decimal(parameter(request,"acknowledgements"),15,acknowledgements)){
            ctx->sendError(request,400,"MIGRATION_EXPLICIT_CONFIRMATION_REQUIRED");return;
        }
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;JsonDocument data;String error;auto review=context(ctx,data,acknowledgements,true);
        if(!storage.prepare(session.c_str(),parameter(request,"revision").c_str(),validateGraph,&review,error)){
            ctx->sendError(request,409,error.c_str());return;
        }
        status(storage,data);ctx->sendSuccess(request,data);
    });
    server->on("/api/config/migration/discard",HTTP_POST,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,true))return;
        if(parameter(request,"confirm")!="DISCARD"||parameter(request,"session").length()!=32){ctx->sendError(request,400,"MIGRATION_EXPLICIT_CONFIRMATION_REQUIRED");return;}
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;String error;
        if(!storage.discard(parameter(request,"session").c_str(),error)){ctx->sendError(request,409,error.c_str());return;}
        JsonDocument data;status(storage,data);ctx->sendSuccess(request,data);
    });
    server->on("/api/config/migration/quarantine",HTTP_POST,[this](AsyncWebServerRequest* request){
        if(!admitted(ctx,request,true))return;
        if(parameter(request,"confirm")!="QUARANTINE"||parameter(request,"revision").length()!=64){ctx->sendError(request,400,"MIGRATION_EXPLICIT_CONFIRMATION_REQUIRED");return;}
        Guard guard;if(!guard){ctx->sendError(request,503,"MIGRATION_BUSY");return;}
        MigrationStorage storage;String error;
        if(!storage.quarantineOrphan(parameter(request,"revision").c_str(),error)){ctx->sendError(request,409,error.c_str());return;}
        JsonDocument data;status(storage,data);ctx->sendSuccess(request,data);
    });
}
#else
void SystemRouteHandler::setupMigrationRoutes(AsyncWebServer*){}
#endif
