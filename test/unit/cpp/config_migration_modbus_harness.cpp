#include "systems/ConfigMigrationModbus.h"
#include <cassert>
#include <string>
using namespace FastBee::ConfigMigration;
static const char* baseline=R"({"enabled":true,"mode":"master","master":{"tasks":[{"enabled":true},{"enabled":false}],"devices":[{"deviceType":"relay","channelCount":2,"coilBase":65534},{"deviceType":"pwm","channelCount":2,"pwmResolution":8},{"deviceType":"pid","channelCount":1},{"deviceType":"motor","channelCount":1}]}})";
int main(int argc,char** argv){
    assert(argc==2);const std::string mode=argv[1];JsonDocument doc;assert(!deserializeJson(doc,baseline));
    ModbusReviewGraph graph;assert(readModbusReviewGraph(doc.as<JsonObjectConst>(),graph));assert(graph.master);
    const auto valid=[&](const char* action,bool enabled=true){return validateMigrationModbusAction(action,strlen(action),graph,enabled)==nullptr;};
    if(mode=="poll"){
        assert(valid("0"));assert(valid(" 0 "));assert(valid("0,1",false));assert(valid(R"({"poll":[0]})"));
        assert(!valid("1"));assert(!valid("2"));assert(!valid("256"));assert(!valid("-1"));assert(!valid("x"));
        assert(!valid("0,"));assert(!valid("0,,1"));assert(!valid("0,0"));assert(!valid(R"({"poll":[true]})"));
        assert(!valid(R"({"poll":["0"]})"));assert(!valid(R"({"poll":[0.5]})"));assert(!valid(R"({"poll":[0,0]})"));
    }else if(mode=="shape"){
        for(const char* invalid:{"{}","[]","{bad}"," {\"poll\":[0]}","{\"poll\":null}","{\"poll\":0}",
            "{\"poll\":[]}","{\"poll\":[0],\"future\":true}","{\"ctrl\":[null]}","{\"ctrl\":[{}]}"})assert(!valid(invalid));
        const char embedded[]={'0','\0','1','\0'};
        assert(validateMigrationModbusAction(embedded,3,graph,true));
    }else if(mode=="relay"){
        assert(valid(R"({"ctrl":[{"d":0,"a":"on","c":1}]})"));
        for(const char* invalid:{R"({"ctrl":[{"d":0,"a":"toggle"}]})",R"({"ctrl":[{"d":256,"a":"on"}]})",
            R"({"ctrl":[{"d":0,"a":"on","c":2}]})",R"({"ctrl":[{"d":0,"a":"on","c":-1}]})",
            R"({"ctrl":[{"d":0,"a":"on","v":0}]})",R"({"ctrl":[{"d":0,"a":"on","unknown":0}]})"})assert(!valid(invalid));
        graph.devices[0].base=65535;assert(!valid(R"({"ctrl":[{"d":0,"a":"on","c":1}]})"));
    }else if(mode=="pwm"){
        assert(valid(R"({"ctrl":[{"d":1,"a":"pwm","c":1,"v":255}]})"));
        assert(!valid(R"({"ctrl":[{"d":1,"a":"pwm","v":256}]})"));
        assert(!valid(R"({"ctrl":[{"d":1,"a":"pwm","v":"12"}]})"));
        assert(!valid(R"({"ctrl":[{"d":1,"a":"pwm"}]})"));
        graph.devices[1].resolution=16;assert(valid(R"({"ctrl":[{"d":1,"a":"pwm","v":65535}]})"));
    }else if(mode=="pid-motor"){
        assert(valid(R"({"ctrl":[{"d":2,"a":"pid","p":"I","v":23},{"d":3,"a":"stop"}]})"));
        assert(valid(R"({"ctrl":[{"d":3,"a":"forward","v":12}]})"));
        assert(!valid(R"({"ctrl":[{"d":3,"a":"unknown"}]})"));
        assert(!valid(R"({"ctrl":[{"d":2,"a":"pid","p":"SV","v":23}]})"));
        assert(!valid(R"({"ctrl":[{"d":2,"a":"pid","v":23}]})"));
    }else if(mode=="disabled"){
        graph.master=false;assert(!valid("0"));assert(valid("0",false));
        assert(!valid(R"({"ctrl":[{"d":0,"a":"on"}]})"));
        assert(valid(R"({"ctrl":[{"d":0,"a":"on"}]})",false));
        graph.master=true;graph.devices[0].enabled=false;
        assert(!valid(R"({"ctrl":[{"d":0,"a":"on"}]})"));
    }else if(mode=="baseline"){
        doc["mode"]=1;assert(!readModbusReviewGraph(doc.as<JsonObjectConst>(),graph));
        assert(!deserializeJson(doc,baseline));doc["master"]["tasks"]=false;assert(!readModbusReviewGraph(doc.as<JsonObjectConst>(),graph));
        assert(!deserializeJson(doc,baseline));doc["master"]["devices"][0]["channelCount"]=-1;assert(!readModbusReviewGraph(doc.as<JsonObjectConst>(),graph));
        assert(!deserializeJson(doc,baseline));doc["master"]["devices"][1]["pwmResolution"]=32;assert(!readModbusReviewGraph(doc.as<JsonObjectConst>(),graph));
        doc.clear();assert(readModbusReviewGraph(doc.as<JsonObjectConst>(),graph));assert(!graph.master&&graph.taskCount==0);
    }else assert(false);
}
