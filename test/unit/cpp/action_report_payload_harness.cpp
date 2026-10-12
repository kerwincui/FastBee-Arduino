#include "core/ActionReportPayload.h"
#include "core/PeripheralExecution.h"
#include <cassert>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    if(mode=="sensor_sample" || mode=="sensor_stale" || mode=="sensor_unknown" || mode=="sensor_unit_lost") {
        ActionExecResult captured;
        assert(FastBee::captureLiteralReportIdentity(captured,"sensor","temperature"));
        captured.actualValue="23.5";captured.success=true;captured.hasSensorSample=true;
        captured.sampleUnit="°C";captured.sampleUnitBytes=captured.sampleUnit.length();
        const uint64_t sampled=UINT64_C(0x100000000)+100;
        captured.sensorSample={sampled,sampled+500,2000,FastBee::SensorSampleQuality::Good};
        if(mode=="sensor_stale") {captured.success=false;captured.sensorSample.quality=FastBee::SensorSampleQuality::Stale;}
        if(mode=="sensor_unknown") {captured.success=false;captured.sensorSample={};}
        if(mode=="sensor_unit_lost") captured.sampleUnit="";
        JsonDocument document;JsonArray rows=document.to<JsonArray>();
        const auto status=FastBee::appendCapturedReportRow(rows,captured);
        if(mode=="sensor_unit_lost") {assert(status==FastBee::CapturedReportRow::Invalid);return 0;}
        assert(status==FastBee::CapturedReportRow::Added);
        assert(rows[0]["sample"]["freshnessAtReceipt"]=="unverified");
        assert(rows[0]["sample"]["unit"]=="°C");
        if(mode=="sensor_sample") {
            assert(rows[0]["sample"]["sampledAtMs"].as<uint64_t>()==sampled);
            assert(rows[0]["sample"]["qualityAtCapture"]=="good");
        } else {
            assert(rows[0]["value"]=="");
            assert(rows[0]["sample"]["qualityAtCapture"]==(mode=="sensor_stale" ? "stale" : "unknown"));
            if(mode=="sensor_unknown") assert(rows[0]["sample"]["sampledAtMs"].isNull());
        }
        size_t emitted=0;
        assert(!FastBee::emitActionReportPayloads(rows,[&](const String& payload){assert(payload.length()<=512);},emitted));
        assert(emitted==1);return 0;
    }
    if(mode=="captured_identity") {
        ActionExecResult captured;
        assert(FastBee::captureLiteralReportIdentity(captured,"old-device","old-property"));
        captured.actualValue="12:34";captured.remark="rtc_calendar_unzoned";captured.success=true;
        JsonDocument document;JsonArray rows=document.to<JsonArray>();
        assert(FastBee::appendCapturedReportRow(rows,captured)==FastBee::CapturedReportRow::Added);
        assert(rows[0]["id"]=="old-property" && rows[0]["value"]=="12:34");
        // Neither a channel-looking value nor a later peripheral ID is allowed
        // to override the field captured before sampling.
        captured.targetPeriphId="new-modbus-device";
        assert(FastBee::appendCapturedReportRow(rows,captured)==FastBee::CapturedReportRow::Added);
        assert(rows[1]["id"]=="old-property");
        return 0;
    }
    if(mode=="captured_failures") {
        ActionExecResult captured;JsonDocument document;JsonArray rows=document.to<JsonArray>();
        assert(FastBee::captureLiteralReportIdentity(captured,"sensor","property"));
        captured.success=false;captured.actualValue="old-good-value";captured.remark="sensor_group_not_published";
        assert(FastBee::appendCapturedReportRow(rows,captured)==FastBee::CapturedReportRow::Added);
        assert(rows[0]["value"]=="" && rows[0]["remark"]=="sensor_group_not_published");
        captured.dataField=""; // Simulates a lost String copy: do not fall back to sensor.
        assert(FastBee::appendCapturedReportRow(rows,captured)==FastBee::CapturedReportRow::Invalid);
        assert(!FastBee::captureLiteralReportIdentity(captured,"","property"));
        assert(FastBee::appendCapturedReportRow(rows,captured)==FastBee::CapturedReportRow::Invalid);
        const std::string oversized(513,'x');
        assert(!FastBee::captureLiteralReportIdentity(captured,"sensor",oversized.c_str()));
        assert(FastBee::captureLiteralReportIdentity(captured,"sensor"));
        captured.targetPeriphId="truncated";
        assert(FastBee::appendCapturedReportRow(rows,captured)==FastBee::CapturedReportRow::Invalid);
        ActionExecResult legacy;
        assert(FastBee::appendCapturedReportRow(rows,legacy)==FastBee::CapturedReportRow::Invalid);
        legacy.reportIdentity=FastBee::ActionReportIdentity::Legacy;
        legacy.targetPeriphId="old-unbound-id";
        assert(FastBee::appendCapturedReportRow(rows,legacy)==FastBee::CapturedReportRow::Invalid);
        return 0;
    }
    if(mode=="captured_peripheral") {
        ActionExecResult captured;JsonDocument document;JsonArray rows=document.to<JsonArray>();
        assert(FastBee::captureLiteralReportIdentity(captured,"rtc"));
        captured.success=true;captured.actualValue="2026-10-03 14:00:00";
        assert(FastBee::appendCapturedReportRow(rows,captured)==FastBee::CapturedReportRow::Added);
        assert(rows[0]["id"]=="rtc" && rows[0]["value"]=="2026-10-03 14:00:00");
        assert(rows[0]["remark"]=="success");return 0;
    }
    JsonDocument source;
    JsonArray rows=source.to<JsonArray>();
    const auto add=[&](const String& value) {
        JsonObject row=rows.add<JsonObject>();
        row["id"]=String(rows.size()); row["value"]=value; row["remark"]="success";
    };
    bool reject=false;
    if(mode=="empty") reject=true;
    else if(mode=="invalid") {rows.add(3);reject=true;}
    else if(mode=="oversize") {add(std::string(513,'x'));reject=true;}
    else if(mode=="late_oversize") {for(int n=0;n<5;++n)add("ok");add(std::string(600,'x'));reject=true;}
    else if(mode=="exact") {
        add(""); const size_t base=measureJson(rows);
        rows[0]["value"]=std::string(512-base,'x'); assert(measureJson(rows)==512);
    } else if(mode=="sd") {
        JsonDocument sd;
        sd["schema"]=1; sd["mounted"]=true; sd["quality"]="lifecycle_snapshot";
        sd["capacityQuality"]="last_operation_snapshot";
        sd["recoverySuggested"]=false;sd["ioFailures"]=UINT32_MAX;sd["recoveries"]=UINT32_MAX;
        sd["totalBytes"]=UINT64_MAX;sd["usedBytes"]=UINT64_MAX;sd["freeBytes"]=UINT64_MAX;
        sd["capacityAgeMs"]=UINT64_MAX;sd["capacitySampledAtMs"]=UINT64_MAX;
        String value;serializeJson(sd,value);
        for(int n=0;n<4;++n){add(value);rows[n]["id"]=std::string(40,'s');rows[n]["remark"]="sd_passive_snapshot";}
    } else if(mode=="escaped") {
        for(int n=0;n<7;++n)add("\"quoted\"\\line\n\t\r"+std::string(70,'x'));
    } else if(mode=="utf8") {
        for(int n=0;n<8;++n)add("\xe4\xb8\xad\xe6\x96\x87"+std::string(85,'x'));
    } else if(mode=="many") {
        for(int n=0;n<105;++n)add(std::string(410,'x'));
    } else if(mode=="multi") {
        for(int n=0;n<7;++n)add(std::string(110,'x'));
    } else {add("small");}
    assert(!source.overflowed());
    std::vector<String> payloads;size_t emitted=999;
    const char* error=FastBee::emitActionReportPayloads(rows,[&](const String& s){payloads.push_back(s);},emitted);
    if(reject){assert(error && emitted==0 && payloads.empty());return 0;}
    assert(!error && emitted==payloads.size() && emitted>0);
    JsonDocument reconstructed;JsonArray collected=reconstructed.to<JsonArray>();
    for(size_t i=0;i<payloads.size();++i){
        assert(payloads[i].length()<=512);
        JsonDocument parsed;assert(!deserializeJson(parsed,payloads[i]));
        JsonArrayConst part;
        if(payloads.size()==1){assert(parsed.is<JsonArray>());part=parsed.as<JsonArrayConst>();}
        else {
            assert(parsed["part"].as<size_t>()==i+1 && parsed["total"].as<size_t>()==payloads.size());
            part=parsed["data"].as<JsonArrayConst>();assert(part.size()>0);
        }
        for(JsonVariantConst row:part)assert(collected.add(row));
    }
    String original,roundtrip;serializeJson(rows,original);serializeJson(collected,roundtrip);
    assert(original==roundtrip);
    if(mode=="exact") assert(payloads[0].length()==512);
    if(mode=="many") assert(payloads.size()==105);
}
