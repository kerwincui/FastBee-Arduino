#include <unity.h>
#include "protocols/RemoteResourceModel.h"
#include "protocols/RemoteCommandState.h"
using namespace FastBee::RemoteResourceModel;

namespace {
JsonDocument parse(const char* text) {
    JsonDocument doc;
    TEST_ASSERT_FALSE(static_cast<bool>(deserializeJson(doc,text)));
    return doc;
}
void test_valid_config() {
    auto config=parse(R"({"remoteChannels":[],"weatherSources":[{"id":"outdoor","label":"天气","enabled":false,"provider":"seniverse","location":"beijing","url":"https://api.seniverse.com/v3/weather/now.json"}]})");
    std::string error;
    TEST_ASSERT_TRUE(validate(config.as<JsonVariantConst>(),error));
    config["weatherSources"][0]["url"]="https://attacker.example/weather";
    TEST_ASSERT_FALSE(validate(config.as<JsonVariantConst>(),error));
}
void test_invalid_configuration_types_and_limits() {
    auto config=parse(R"({"remoteChannels":[],"weatherSources":[]})");
    std::string error;
    TEST_ASSERT_TRUE(validate(config.as<JsonVariantConst>(),error));
    config["remoteChannels"]="[]";
    TEST_ASSERT_FALSE(validate(config.as<JsonVariantConst>(),error));
    auto channels=config["remoteChannels"].to<JsonArray>();
    channels.add<JsonObject>()["id"]="retired";
    TEST_ASSERT_FALSE(validate(config.as<JsonVariantConst>(),error));
    channels.clear();
    auto weather=config["weatherSources"].to<JsonArray>();
    fixedWeather(weather.add<JsonObject>(),JsonObjectConst());
    fixedWeather(weather.add<JsonObject>(),JsonObjectConst());
    TEST_ASSERT_FALSE(validate(config.as<JsonVariantConst>(),error));
}
void test_strict_numeric_values() {
    auto values=parse(R"([0,"0","-12.50"," 2.0 ",null,"",true,"broken","22 C","NaN","Infinity",{},[]])");
    double value=123;
    for (unsigned i=0;i<4;++i) TEST_ASSERT_TRUE(number(values[i],value));
    for (unsigned i=4;i<values.size();++i) TEST_ASSERT_FALSE(number(values[i],value));
}
void test_field_paths_are_data_only() {
    auto body=parse(R"({"results":[{"now":{"temperature":"26"}}]})");
    double value=0;
    TEST_ASSERT_TRUE(number(field(body.as<JsonVariantConst>(),"results.0.now.temperature"),value));
    TEST_ASSERT_TRUE_MESSAGE(value == 26.0, "Field mapping must preserve the parsed double value");
    TEST_ASSERT_TRUE(field(body.as<JsonVariantConst>(),"results.3.now.temperature").isNull());
    TEST_ASSERT_TRUE(field(body.as<JsonVariantConst>(),"results.x.now.temperature").isNull());
    TEST_ASSERT_FALSE(pathValid("results[0].now"));
    TEST_ASSERT_FALSE(pathValid("a..b"));
    TEST_ASSERT_FALSE(pathValid("a."));
    TEST_ASSERT_FALSE(pathValid("a();"));
}
void test_seniverse_current_visual_mapping() {
    auto source=parse(R"({"provider":"seniverse"})");
    auto body=parse(R"({"results":[{"location":{"name":"北京"},"now":{"text":"晴","code":"1","temperature":"18"},"last_update":"2026-09-17T12:00:00+08:00"}]})");
    JsonDocument result; std::string error;
    TEST_ASSERT_TRUE(parseWeather(source.as<JsonObjectConst>(),body.as<JsonVariantConst>(),result.to<JsonObject>(),error));
    TEST_ASSERT_TRUE_MESSAGE(result["temperature"].as<double>() == 18.0,
                             "Weather mapping must preserve the parsed double temperature");
    TEST_ASSERT_EQUAL_STRING("clear-night",result["condition"].as<const char*>());
    TEST_ASSERT_TRUE(result["isNight"].as<bool>());
    TEST_ASSERT_TRUE(result["humidity"].isNull());
    body["results"][0]["now"]["temperature"]="bad";
    TEST_ASSERT_FALSE(parseWeather(source.as<JsonObjectConst>(),body.as<JsonVariantConst>(),result.to<JsonObject>(),error));
    body["status_code"]="AP010006";
    TEST_ASSERT_FALSE(parseWeather(source.as<JsonObjectConst>(),body.as<JsonVariantConst>(),result.to<JsonObject>(),error));
}
void test_custom_weather_mapping_unknown_is_not_sunny() {
    auto source=parse(R"({"provider":"custom","fields":{"temperature":"temp"}})");
    auto body=parse(R"({"temp":25})");
    JsonDocument result; std::string error;
    TEST_ASSERT_FALSE(parseWeather(source.as<JsonObjectConst>(),body.as<JsonVariantConst>(),result.to<JsonObject>(),error));
}
void test_forecast_is_bounded_and_rejects_invalid_points() {
    auto source=parse(R"({"provider":"seniverse"})");
    auto body=parse(R"({"results":[{"daily":[{"low":12,"high":20}]}]})");
    JsonDocument result; std::string error;
    TEST_ASSERT_FALSE(parseWeather(source.as<JsonObjectConst>(),body.as<JsonVariantConst>(),result.to<JsonObject>(),error,true));
}
void test_custom_forecast_fields_are_independently_mapped() {
    JsonDocument config;
    config["remoteChannels"].to<JsonArray>();
    auto weather=config["weatherSources"].to<JsonArray>().add<JsonObject>();
    fixedWeather(weather,JsonObjectConst());
    std::string error;
    TEST_ASSERT_TRUE(validate(config.as<JsonVariantConst>(),error));
    for (const char* retired : {"forecastEnabled","fields","authHeader","apiKeyParam"}) {
        weather[retired]=false;
        TEST_ASSERT_FALSE(validate(config.as<JsonVariantConst>(),error));
        weather.remove(retired);
    }
}
void test_weather_preview_is_bounded_and_secret_free() {
    auto normalized=parse(R"({"temperature":"21.5","condition":"rain","city":"杭州","description":"小雨","humidity":73,"apiKey":"must-not-leak","raw":{"token":"hidden"},"forecast":[{"date":"09-20","low":18,"high":24,"condition":"rain","secret":"hidden"},{"date":"09-21","low":19,"high":25,"condition":"provider-code"},{"date":"09-22","low":20,"high":26,"condition":"sunny"},{"date":"09-23","low":21,"high":27,"condition":"sunny"}]})");
    JsonDocument result;
    copyWeatherPreview(normalized.as<JsonVariantConst>(),result.to<JsonObject>());
    TEST_ASSERT_TRUE(result["temperature"].as<double>() == 21.5);
    TEST_ASSERT_EQUAL_STRING("rain",result["condition"].as<const char*>());
    TEST_ASSERT_TRUE(result["forecast"].isNull());
    TEST_ASSERT_TRUE(result["apiKey"].isNull());
    TEST_ASSERT_TRUE(result["raw"].isNull());
    TEST_ASSERT_TRUE(result["forecast"][0]["secret"].isNull());
    std::string serialized; serializeJson(result,serialized);
    TEST_ASSERT_TRUE(serialized.find("must-not-leak")==std::string::npos);
    TEST_ASSERT_TRUE(serialized.find("hidden")==std::string::npos);
}
void test_source_addresses_and_icons() {
    TEST_ASSERT_TRUE(urlValid("https://weather.example/api?location=beijing"));
    TEST_ASSERT_TRUE(urlValid("http://192.168.1.5/weather"));
    TEST_ASSERT_FALSE(urlValid("https://user:secret@weather.example/api"));
    TEST_ASSERT_FALSE(urlValid("file:///config/users.json"));
    TEST_ASSERT_FALSE(urlValid("https:///missing"));
    TEST_ASSERT_FALSE(urlValid("https://example/\r\nInjected"));
    TEST_ASSERT_TRUE(queryNameValid("api-key.v2"));
    TEST_ASSERT_FALSE(queryNameValid("api key"));
    TEST_ASSERT_FALSE(queryNameValid("key=value"));
    auto invalidQueryName=parse(R"({"remoteChannels":[],"weatherSources":[{"id":"custom","label":"Custom","enabled":false,"provider":"custom","url":"https://example.test/weather","apiKeyParam":42,"fields":{"temperature":"now.temp"}}]})");
    std::string queryError;
    TEST_ASSERT_FALSE(validate(invalidQueryName.as<JsonVariantConst>(),queryError));
    TEST_ASSERT_FALSE(topicValid("/device/http/upgrade/set"));
    TEST_ASSERT_EQUAL_STRING("snow",condition("23"));
    TEST_ASSERT_EQUAL_STRING("unknown",condition("99"));
    TEST_ASSERT_EQUAL_STRING("unknown",condition("unknown"));
    TEST_ASSERT_EQUAL_STRING("unknown",condition(""));
}
void test_mqtt_filters_only_match_their_namespace() {
    TEST_ASSERT_TRUE(topicMatches("house/+/state","house/lamp/state"));
    TEST_ASSERT_FALSE(topicMatches("house/+/state","factory/lamp/state"));
    TEST_ASSERT_TRUE(topicMatches("house/#","house"));
    TEST_ASSERT_TRUE(topicMatches("house/#","house/lamp/state"));
    TEST_ASSERT_FALSE(topicMatches("house/+","house/lamp/state"));
    TEST_ASSERT_FALSE(topicMatches("house/+","house"));
    TEST_ASSERT_TRUE(topicMatches("house/+","house/"));
    TEST_ASSERT_FALSE(topicMatches("#","$SYS/status"));
    TEST_ASSERT_FALSE(topicMatches("/function/get","/device/function/get"));
}
void test_switch_needs_confirmed_readback() {
    FastBee::RemoteCommandState state;
    TEST_ASSERT_TRUE(state.enqueue(true,true,100));
    TEST_ASSERT_FALSE(state.enqueue(false,true,101));
    state.receiveState(true); // Receipt before publish cannot confirm the command.
    TEST_ASSERT_EQUAL_STRING("queued",state.result);
    TEST_ASSERT_TRUE(state.takeForSend(false,false,110));
    state.published(true,false);
    TEST_ASSERT_TRUE(state.waiting);
    state.receiveState(false);
    TEST_ASSERT_TRUE(state.waiting);
    state.receiveState(true);
    TEST_ASSERT_FALSE(state.waiting);
    TEST_ASSERT_EQUAL_STRING("confirmed",state.result);
}
void test_disconnect_never_replays_commands() {
    FastBee::RemoteCommandState state;
    TEST_ASSERT_FALSE(state.enqueue(true,false,100));
    TEST_ASSERT_TRUE(state.enqueue(true,true,200));
    state.disconnect();
    TEST_ASSERT_FALSE(state.takeForSend(false,false,210));
    TEST_ASSERT_EQUAL_STRING("connection_lost",state.result);
    TEST_ASSERT_TRUE(state.enqueue(true,true,300));
    TEST_ASSERT_TRUE(state.takeForSend(false,false,310));
    state.disconnect();
    TEST_ASSERT_FALSE(state.waiting);
    TEST_ASSERT_FALSE(state.takeForSend(false,false,320));
}
void test_command_expiry_timeout_and_failure() {
    FastBee::RemoteCommandState state;
    TEST_ASSERT_TRUE(state.enqueue(true,true,1));
    TEST_ASSERT_FALSE(state.takeForSend(false,false,2002));
    TEST_ASSERT_EQUAL_STRING("expired_before_send",state.result);
    TEST_ASSERT_TRUE(state.enqueue(true,true,3000));
    TEST_ASSERT_FALSE(state.takeForSend(false,true,3001));
    TEST_ASSERT_TRUE(state.enqueue(true,true,4000));
    TEST_ASSERT_TRUE(state.takeForSend(false,false,4001));
    state.published(false,false);
    TEST_ASSERT_EQUAL_STRING("send_failed",state.result);
    TEST_ASSERT_FALSE(state.waiting);
    TEST_ASSERT_TRUE(state.enqueue(true,true,UINT32_MAX-100));
    TEST_ASSERT_TRUE(state.takeForSend(false,false,UINT32_MAX-50));
    state.published(true,false);
    state.checkTimeout(1050,1000);
    TEST_ASSERT_FALSE(state.waiting);
    TEST_ASSERT_EQUAL_STRING("result_unknown",state.result);
}
void test_scene_publish_does_not_claim_execution() {
    FastBee::RemoteCommandState state;
    TEST_ASSERT_TRUE(state.enqueue(true,true,10));
    TEST_ASSERT_TRUE(state.takeForSend(true,false,20));
    state.published(true,true);
    TEST_ASSERT_FALSE(state.waiting);
    TEST_ASSERT_EQUAL_STRING("sent_unconfirmed",state.result);
}
void test_mapping_revision_changes_without_leaking_secrets() {
    auto config=parse(R"({"provider":"custom","url":"https://example/weather","location":"home","label":"Weather","apiKey":"private","fields":{"temperature":"now.temperature"}})");
    const auto first=sourceRevision(config.as<JsonObjectConst>());
    TEST_ASSERT_EQUAL_UINT(8,first.size());
    config["label"]="Renamed";
    config["apiKey"]="rotated";
    TEST_ASSERT_TRUE(first==sourceRevision(config.as<JsonObjectConst>()));
    config["fields"]["temperature"]="different.temperature";
    TEST_ASSERT_FALSE(first==sourceRevision(config.as<JsonObjectConst>()));
    config["fields"]["temperature"]="now.temperature";
    config["url"]="https://example/another-location";
    TEST_ASSERT_FALSE(first==sourceRevision(config.as<JsonObjectConst>()));
}
void test_migration_preserves_binding_and_secret_without_public_leak() {
    auto legacy=parse(R"({"mqtt":{"host":"keep-me"},"remoteChannels":[{"id":"lamp","label":"Lamp","commandTopic":"secret-topic"}],"weatherSources":[{"id":"outdoor","provider":"seniverse","enabled":true,"apiKey":"private-key","location":"shanghai","intervalSeconds":900,"fields":{"foo":"bar"},"forecastEnabled":true}]})");
    JsonDocument active,report;
    migrateWeather(legacy.as<JsonVariantConst>(),active.to<JsonObject>(),report.to<JsonObject>());
    TEST_ASSERT_FALSE(report["required"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("outdoor",active["weatherSources"][0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("private-key",active["weatherSources"][0]["apiKey"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT(900,active["weatherSources"][0]["intervalSeconds"]);
    TEST_ASSERT_TRUE(active["weatherSources"][0]["forecastEnabled"].isNull());
    TEST_ASSERT_EQUAL_UINT(0,active["remoteChannels"].size());
    std::string publicData; serializeJson(report,publicData);
    TEST_ASSERT_TRUE(publicData.find("private-key")==std::string::npos);
    TEST_ASSERT_TRUE(publicData.find("secret-topic")==std::string::npos);
    TEST_ASSERT_EQUAL_STRING("keep-me",legacy["mqtt"]["host"].as<const char*>());
    JsonDocument twice,secondReport;
    migrateWeather(active.as<JsonVariantConst>(),twice.to<JsonObject>(),secondReport.to<JsonObject>());
    TEST_ASSERT_TRUE(active.as<JsonVariantConst>()==twice.as<JsonVariantConst>());
    legacy["weatherSources"][0]["apiKey"]="";
    JsonDocument invalid,repair;
    migrateWeather(legacy.as<JsonVariantConst>(),invalid.to<JsonObject>(),repair.to<JsonObject>());
    TEST_ASSERT_TRUE(repair["required"].as<bool>());
    TEST_ASSERT_FALSE(invalid["weatherSources"][0]["enabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("outdoor",repair["candidates"][0]["id"].as<const char*>());

}
void test_ambiguous_migration_requires_explicit_selection() {
    auto legacy=parse(R"({"remoteChannels":[],"weatherSources":[{"id":"first","provider":"seniverse","enabled":true,"apiKey":"one","location":"a"},{"id":"second","provider":"seniverse","enabled":true,"apiKey":"two","location":"b"}]})");
    JsonDocument active,report,next;
    migrateWeather(legacy.as<JsonVariantConst>(),active.to<JsonObject>(),report.to<JsonObject>());
    TEST_ASSERT_TRUE(report["required"].as<bool>());
    TEST_ASSERT_FALSE(active["weatherSources"][0]["enabled"].as<bool>());
    auto submitted=parse(R"({"expectedRevision":7,"enabled":true,"location":"b","intervalSeconds":1800,"apiKey":""})");
    std::string error;
    TEST_ASSERT_FALSE(prepareScreenWeather(submitted.as<JsonVariantConst>(),active["weatherSources"][0],legacy.as<JsonVariantConst>(),true,7,next.to<JsonObject>(),error));
    submitted["legacySourceId"]="second";
    TEST_ASSERT_TRUE(prepareScreenWeather(submitted.as<JsonVariantConst>(),active["weatherSources"][0],legacy.as<JsonVariantConst>(),true,7,next.to<JsonObject>(),error));
    TEST_ASSERT_EQUAL_STRING("second",next["weatherSources"][0]["id"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("two",next["weatherSources"][0]["apiKey"].as<const char*>());
    submitted["legacySourceId"]="missing";
    TEST_ASSERT_FALSE(prepareScreenWeather(submitted.as<JsonVariantConst>(),active["weatherSources"][0],legacy.as<JsonVariantConst>(),true,7,next.to<JsonObject>(),error));
}
void test_weather_save_conflict_blank_key_clear_and_retired_fields() {
    JsonDocument current,next;
    fixedWeather(current.to<JsonObject>(),JsonObjectConst()); current["apiKey"]="keep-key";
    auto submitted=parse(R"({"expectedRevision":2,"enabled":true,"location":"beijing","intervalSeconds":1800,"apiKey":""})");
    std::string error;
    auto prepare=[&]() { return prepareScreenWeather(submitted.as<JsonVariantConst>(),current.as<JsonObjectConst>(),JsonVariantConst(),false,2,next.to<JsonObject>(),error); };
    TEST_ASSERT_TRUE(prepare()); TEST_ASSERT_EQUAL_STRING("keep-key",next["weatherSources"][0]["apiKey"].as<const char*>());
    submitted["expectedRevision"]=1; TEST_ASSERT_FALSE(prepare());
    submitted["expectedRevision"]=2; submitted["clearApiKey"]=true; TEST_ASSERT_FALSE(prepare());
    submitted["enabled"]=false; TEST_ASSERT_TRUE(prepare()); TEST_ASSERT_EQUAL_STRING("",next["weatherSources"][0]["apiKey"].as<const char*>());
    submitted["url"]="https://attacker.example"; TEST_ASSERT_FALSE(prepare()); submitted.remove("url");
    submitted["clearApiKey"]=false; submitted["apiKey"]="new-key"; TEST_ASSERT_TRUE(prepare());
    TEST_ASSERT_EQUAL_STRING("new-key",next["weatherSources"][0]["apiKey"].as<const char*>());
    submitted["apiKey"]="line\nbreak"; TEST_ASSERT_FALSE(prepare());
    submitted["apiKey"]=""; submitted["intervalSeconds"]=1; TEST_ASSERT_FALSE(prepare());
}

}
void test_remote_resources_group() {
    RUN_TEST(test_migration_preserves_binding_and_secret_without_public_leak);
    RUN_TEST(test_ambiguous_migration_requires_explicit_selection);
    RUN_TEST(test_weather_save_conflict_blank_key_clear_and_retired_fields);
    RUN_TEST(test_valid_config);
    RUN_TEST(test_invalid_configuration_types_and_limits);
    RUN_TEST(test_strict_numeric_values);
    RUN_TEST(test_field_paths_are_data_only);
    RUN_TEST(test_seniverse_current_visual_mapping);
    RUN_TEST(test_custom_weather_mapping_unknown_is_not_sunny);
    RUN_TEST(test_forecast_is_bounded_and_rejects_invalid_points);
    RUN_TEST(test_custom_forecast_fields_are_independently_mapped);
    RUN_TEST(test_weather_preview_is_bounded_and_secret_free);
    RUN_TEST(test_source_addresses_and_icons);
    RUN_TEST(test_mqtt_filters_only_match_their_namespace);
    RUN_TEST(test_switch_needs_confirmed_readback);
    RUN_TEST(test_disconnect_never_replays_commands);
    RUN_TEST(test_command_expiry_timeout_and_failure);
    RUN_TEST(test_scene_publish_does_not_claim_execution);
    RUN_TEST(test_mapping_revision_changes_without_leaking_secrets);
}
#ifdef FASTBEE_REMOTE_MODEL_STANDALONE
void setUp() {}
void tearDown() {}
int main() {
    UNITY_BEGIN();
    test_remote_resources_group();
    return UNITY_END();
}
#endif
