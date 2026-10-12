#include <cassert>
#include <cstring>
#include "core/PeriphExecConfigValidation.h"
int main() {
    PeriphExecRule rule; rule.id="rtc_rule";rule.name="RTC clock control";
    rule.triggers.emplace_back();rule.actions.emplace_back();
    auto& action=rule.actions[0];
    action.actionType=static_cast<uint8_t>(ExecActionType::ACTION_RTC_CONTROL);
    action.targetPeriphId="rtc_01";action.actionValue="stop";action.useReceivedValue=false;
    assert(action.actionType==35 && !FastBee::PeriphExecConfig::validateRule(rule));
    action.actionType=static_cast<uint8_t>(ExecActionType::ACTION_RTC_READ);action.actionValue="";
    assert(action.actionType==36 && !FastBee::PeriphExecConfig::validateRule(rule));
    action.actionType=37;
    action.actionValue="system-local";
    assert(!FastBee::PeriphExecConfig::validateRule(rule));
    action.actionType=38;
    action.actionValue="";
    assert(!FastBee::PeriphExecConfig::validateRule(rule));
    action.actionType=39;
    assert(!FastBee::PeriphExecConfig::validateRule(rule));
    action.actionType=40;
    assert(!FastBee::PeriphExecConfig::validateRule(rule));
    action.actionType=41;
    assert(std::strcmp(FastBee::PeriphExecConfig::validateRule(rule),"CONFIG_ACTION_TYPE")==0);
    JsonDocument doc;
    const auto error=deserializeJson(doc,R"({"rules":[{"id":"rtc_rule","name":"RTC","triggers":[{}],"actions":[{"actionType":35,"targetPeriphId":"rtc_01","actionValue":"stop","useReceivedValue":false}]}]})");
    assert(!error && !FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    doc["rules"][0]["actions"][0]["actionType"]=36;
    doc["rules"][0]["actions"][0]["actionValue"]="";
    assert(!FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    doc["rules"][0]["actions"][0]["actionType"]=37;
    doc["rules"][0]["actions"][0]["actionValue"]="system-local";
    assert(!FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    doc["rules"][0]["actions"][0]["actionType"]=38;
    doc["rules"][0]["actions"][0]["actionValue"]="";
    assert(!FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    doc["rules"][0]["actions"][0]["actionType"]=39;
    assert(!FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    doc["rules"][0]["actions"][0]["actionType"]=40;
    assert(!FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()));
    doc["rules"][0]["actions"][0]["actionType"]=41;
    assert(std::strcmp(FastBee::PeriphExecConfig::validateDocument(doc.as<JsonVariantConst>()),"CONFIG_ACTION_TYPE")==0);
}
