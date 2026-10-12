#include <cassert>
#include "network/NetworkResourcePins.h"
int main() {
 using namespace FastBee;
 JsonDocument doc; EthernetConfig config; String error;
 const char* invalid[]={"null","true","false","11.5","267","-245","2147483648",
                       "\"267\"","\"-245\"","\"11.0\"","\"1e1\"","\" 11\"","\"+11\"","\"\"","{}","[]"};
 for(const char* value:invalid) {
  doc.clear(); auto json=std::string("{\"spiMosi\":7,\"csPin\":")+value+"}";
  assert(!deserializeJson(doc,json)); const auto saved=config;
  assert(!parseEthernetResourcePins(doc.as<JsonObjectConst>(),config,error));
  assert(config.spiMosi==saved.spiMosi && config.csPin==saved.csPin);
 }
 doc.clear(); assert(!deserializeJson(doc,"{\"spiMosi\":\"7\",\"csPin\":10,\"rstPin\":-1,\"intPin\":\"-1\"}"));
 assert(parseEthernetResourcePins(doc.as<JsonObjectConst>(),config,error));
 assert(config.spiMosi==7 && config.csPin==10 && config.rstPin==-1 && config.intPin==-1 && error.isEmpty());
 CellularConfig cellular;
 for(const char* value:invalid) {
  doc.clear(); auto json=std::string("{\"rxPin\":7,\"txPin\":")+value+"}";
  assert(!deserializeJson(doc,json)); const auto saved=cellular;
  assert(!parseCellularResourcePins(doc.as<JsonObjectConst>(),cellular,error));
  assert(cellular.rxPin==saved.rxPin && cellular.txPin==saved.txPin && cellular.pwrPin==saved.pwrPin);
 }
 doc.clear(); assert(!deserializeJson(doc,"{\"rxPin\":\"7\",\"txPin\":8,\"pwrPin\":\"-1\"}"));
 assert(parseCellularResourcePins(doc.as<JsonObjectConst>(),cellular,error));
 assert(cellular.rxPin==7 && cellular.txPin==8 && cellular.pwrPin==-1 && error.isEmpty());
 for(const char* pin : {"rxPin","txPin","pwrPin"}) {
  doc.clear(); doc[pin]=-2;
  assert(!parseCellularResourcePins(doc.as<JsonObjectConst>(),cellular,error));
 }
 doc.clear(); doc["spiMiso"]=-1;
 assert(!parseEthernetResourcePins(doc.as<JsonObjectConst>(),config,error));
 doc.clear(); doc.to<JsonObject>();
 assert(parseEthernetResourcePins(doc.as<JsonObjectConst>(),config,error));
}
