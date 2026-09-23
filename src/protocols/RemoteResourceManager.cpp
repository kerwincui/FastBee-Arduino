#include "protocols/RemoteResourceManager.h"
#if FASTBEE_ENABLE_REMOTE_RESOURCES
#include "protocols/RemoteResourceModel.h"
#include "protocols/MQTTClient.h"
#include "systems/ConfigStorage.h"
#include "core/FastBeeFramework.h"
#include "core/PeriphExecManager.h"
#include "network/NetworkManager.h"
#include "network/OTAManager.h"
#include "network/ProtocolSecret.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <cctype>
#include <cmath>
#include <esp_heap_caps.h>
#include <freertos/task.h>
#include <time.h>

namespace {
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");
class Guard {
    SemaphoreHandle_t mutex_;
public:
    explicit Guard(SemaphoreHandle_t m) : mutex_(m) { if (mutex_) xSemaphoreTakeRecursive(mutex_,portMAX_DELAY); }
    ~Guard() { if (mutex_) xSemaphoreGiveRecursive(mutex_); }
};
bool due(uint32_t now,uint32_t deadline) { return !deadline || static_cast<int32_t>(now-deadline)>=0; }
constexpr size_t WEATHER_WORKER_MIN_INTERNAL_BYTES=24000U;
constexpr uint32_t WEATHER_TRANSIENT_RETRY_MIN_MS=30000U;
constexpr uint32_t WEATHER_TRANSIENT_RETRY_MAX_MS=300000U;

bool weatherWorkerEligible(bool networkConnected,bool workerActive,bool hasSources,
                           bool otaInProgress,bool ready,size_t internalFree) {
    return networkConnected && !workerActive && hasSources && !otaInProgress &&
           ready && internalFree>=WEATHER_WORKER_MIN_INTERNAL_BYTES;
}

uint32_t weatherFailureDelayMs(uint8_t failures,bool transientTransportFailure,
                               uint32_t configuredIntervalMs) {
    const uint8_t attempts=failures ? failures : 1U;
    if (transientTransportFailure) {
        const uint8_t shift=std::min<uint8_t>(attempts-1U,4U);
        return std::min<uint32_t>(WEATHER_TRANSIENT_RETRY_MIN_MS<<shift,
                                  WEATHER_TRANSIENT_RETRY_MAX_MS);
    }
    const uint8_t shift=std::min<uint8_t>(attempts,6U);
    return std::max<uint32_t>(configuredIntervalMs,60000U<<shift);
}

bool weatherNetworkConnected() {
    auto* fw=FastBeeFramework::getInstance();
    auto* network=fw ? static_cast<FBNetworkManager*>(fw->getNetworkManager()) : nullptr;
    return network && network->isNetworkConnected();
}

bool otaBusy() {
#if FASTBEE_ENABLE_OTA
    auto* fw=FastBeeFramework::getInstance();
    return fw && fw->getOTAManager() && fw->getOTAManager()->isOTAInProgress();
#else
    return false;
#endif
}
String escaped(const String& text) {
    String out; out.reserve(text.length()*2);
    const char* hex="0123456789ABCDEF";
    for (size_t i=0;i<text.length();++i) {
        uint8_t c=text[i];
        if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') out+=char(c);
        else { out+='%'; out+=hex[c>>4]; out+=hex[c&15]; }
    }
    return out;
}
class BoundedResponse : public Stream {
    uint32_t started_=millis();
public:
    String body;
    bool exceeded=false;
    BoundedResponse() { body.reserve(2048); }
    size_t write(uint8_t b) override { return write(&b,1); }
    size_t write(const uint8_t* bytes,size_t len) override {
        if (millis()-started_>15000U || body.length()+len>FastBee::RemoteResourceModel::MAX_BODY) { exceeded=true; return 0; }
        return body.concat(reinterpret_cast<const char*>(bytes),len) ? len : 0;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};
bool fetchWeather(JsonObjectConst source,JsonDocument& normalized,String& error) {
    auto* fw=FastBeeFramework::getInstance();
    auto* network=fw ? static_cast<FBNetworkManager*>(fw->getNetworkManager()) : nullptr;
    if (!network || !network->isNetworkConnected()) { error="Network offline"; return false; }
    if (network->getNetworkType()==NetworkType::NET_4G) { error="Weather HTTP transport is unavailable on cellular"; return false; }
    // Fixed HTTPS endpoint: imported configuration can never redirect credentials.
    String url="https://api.seniverse.com/v3/weather/now.json";
    if (time(nullptr)<1577836800) { error="等待系统时间同步，联网后自动更新"; return false; }
    if (otaBusy()) { error="Paused during OTA"; return false; }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<65000U) { error="Insufficient memory for weather request"; return false; }
    url+="?key="+escaped(source["apiKey"] | "");
    url+="&location="+escaped(source["location"] | "beijing")+"&language=zh-Hans&unit=c";
    WiFiClientSecure secure;
    HTTPClient http;
    {
        const size_t caSize=x509_crt_bundle_end-x509_crt_bundle_start;
        if (!caSize) { error="CA bundle unavailable"; return false; }
        secure.setCACertBundle(x509_crt_bundle_start,caSize);
        secure.setHandshakeTimeout(10);
    }
    if (!http.begin(static_cast<NetworkClient&>(secure),url)) { error="Cannot open weather request"; return false; }
    http.setConnectTimeout(5000);
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setReuse(false);
    http.addHeader("Accept","application/json");
    const int status=http.GET();
    if (status!=200) {
        error=status==401 || status==403 ? "Weather authentication or permission denied" : status==429 ? "Weather rate limit reached" : status<0 ? "Weather network request failed" : String("Weather HTTP status ")+status;
        http.end(); return false;
    }
    if (http.getSize()>static_cast<int>(FastBee::RemoteResourceModel::MAX_BODY)) { error="Weather response exceeds 8 KiB"; http.end(); return false; }
    BoundedResponse response;
    const int received=http.writeToStream(&response);
    http.end();
    if (received<0 || response.exceeded || !response.body.length()) { error="Weather response incomplete or too large"; return false; }
    JsonDocument parsed;
    auto e=deserializeJson(parsed,response.body,DeserializationOption::NestingLimit(12));
    if (e) { error="Weather response is invalid JSON"; return false; }
    std::string parseError;
    bool ok=FastBee::RemoteResourceModel::parseWeather(source,parsed.as<JsonVariantConst>(),normalized.to<JsonObject>(),parseError);
    error=parseError.c_str(); return ok;
}
}

namespace FastBee {
RemoteResourceManager& RemoteResourceManager::instance() { static RemoteResourceManager manager; return manager; }

bool RemoteResourceManager::initialize() {
    if (!mutex_) mutex_=xSemaphoreCreateRecursiveMutex();
    if (!mutex_) return false;
    Guard lock(mutex_);
    if (activeExternalCommits_) return false;
    JsonDocument all;
    if (!ConfigStorage::getInstance().loadJSONConfig("/config/protocol.json",all)) return false;
    weatherRevision_ = all["screenWeatherRevision"] | 1U;
    weatherMigration_.clear();
    JsonDocument sources;
    RemoteResourceModel::migrateWeather(all.as<JsonVariantConst>(),sources.to<JsonObject>(),weatherMigration_.to<JsonObject>());
    const bool migrated = (all["screenWeatherVersion"] | 0) == 1;
    // The backup precedes the single atomic protocol write. No screen/rule files
    // are rewritten here, so a failed write simply retries on next initialization.
    if (!migrated && (all["weatherSources"].size() || all["remoteChannels"].size())) {
        const char* backup = "/config/protocol.pre-screen-weather.json";
        if (!LittleFS.exists(backup) && !ConfigStorage::getInstance().saveJSONConfig(backup, all)) return false;
    }
    for (JsonObjectConst channel : all["remoteChannels"].as<JsonArrayConst>()) {
#if FASTBEE_ENABLE_PERIPH_EXEC
        PeriphExecManager::getInstance().invalidateObservation(
            String("remote.")+(channel["id"] | ""),"Source retired",String());
#endif
        const String topic = channel["stateTopic"] | "";
        if (topic.length() && retiredTopics_.size() < 8 &&
            std::find(retiredTopics_.begin(),retiredTopics_.end(),topic)==retiredTopics_.end()) retiredTopics_.push_back(topic);
    }
    if (!migrated && !(weatherMigration_["required"] | false)) {
        all["remoteChannels"] = sources["remoteChannels"]; all["weatherSources"] = sources["weatherSources"];
        all["screenWeatherVersion"] = 1; all["screenWeatherRevision"] = weatherRevision_;
        all["screenWeatherMigration"] = weatherMigration_;
        if (!ConfigStorage::getInstance().saveJSONConfig("/config/protocol.json",all)) return false;
    } else if (migrated && all["screenWeatherMigration"].is<JsonObjectConst>()) {
        weatherMigration_.set(all["screenWeatherMigration"]);
    }
    std::string error;
    if (!RemoteResourceModel::validate(sources.as<JsonVariantConst>(),error)) return false;
    applyConfig(sources.as<JsonVariantConst>());
    return true;
}

void RemoteResourceManager::getScreenWeather(JsonObject out) {
    Guard lock(mutex_);
    const auto source = config_["weatherSources"][0].as<JsonObjectConst>();
    out["revision"] = weatherRevision_; out["id"] = source["id"] | "local";
    out["enabled"] = source["enabled"] | false; out["location"] = source["location"] | "beijing";
    out["intervalSeconds"] = source["intervalSeconds"] | 1800U;
    out["hasApiKey"] = strlen(source["apiKey"] | "") != 0;
    out["migration"] = weatherMigration_;
    JsonObject status = out["status"].to<JsonObject>();
    if (!getWeatherTestStatus(String(source["id"] | "local"), status)) status["status"] = "disabled";
}

bool RemoteResourceManager::saveScreenWeather(JsonVariantConst submitted, String& error) {
    if (!mutex_) { error = "Weather settings unavailable"; return false; }
    Guard lock(mutex_);
    JsonDocument legacy;
    if ((weatherMigration_["required"] | false) &&
        !ConfigStorage::getInstance().loadJSONConfig("/config/protocol.json",legacy)) {
        error="Cannot load legacy weather"; return false;
    }
    JsonDocument next;
    std::string validation;
    if (!RemoteResourceModel::prepareScreenWeather(submitted,config_["weatherSources"][0],
            legacy.as<JsonVariantConst>(),weatherMigration_["required"] | false,
            weatherRevision_,next.to<JsonObject>(),validation)) { error=validation.c_str(); return false; }
    next["expectedWeatherRevision"] = weatherRevision_;
    if (!saveConfig(next.as<JsonVariantConst>(),error)) return false;
    weatherMigration_["required"] = false; weatherMigration_.remove("candidates"); return true;
}

void RemoteResourceManager::applyConfig(JsonVariantConst config) {
    // A repeated form save must not reset freshness, manual rate limits or a
    // request already in flight when the source settings have not changed.
    if (config_.as<JsonVariantConst>() == config) return;
#if FASTBEE_ENABLE_PERIPH_EXEC
    for (const char* collection : {"remoteChannels", "weatherSources"}) {
        for (JsonObjectConst previous : config_[collection].as<JsonArrayConst>()) {
            if (!(previous["enabled"] | false)) continue;
            const String id = previous["id"] | "";
            JsonObjectConst next;
            for (JsonObjectConst candidate : config[collection].as<JsonArrayConst>()) {
                if (id == (candidate["id"] | "") && (candidate["enabled"] | false)) { next = candidate; break; }
            }
            const String previousRevision = RemoteResourceModel::sourceRevision(previous).c_str();
            const String nextRevision = next.isNull() ? String() : String(RemoteResourceModel::sourceRevision(next).c_str());
            if (previousRevision == nextRevision) continue;
            const String reason = next.isNull() ? "Source removed or disabled" : "Source mapping changed";
            if (!strcmp(collection, "remoteChannels")) {
                PeriphExecManager::getInstance().invalidateObservation("remote." + id, reason, nextRevision);
            } else {
                for (const char* field : {"temperature", "humidity"})
                    PeriphExecManager::getInstance().invalidateObservation("weather." + id + "." + field, reason, nextRevision);
            }
        }
    }
#endif
    config_.set(config); ++generation_; weather_.clear();
    for (JsonObjectConst w:config_["weatherSources"].as<JsonArrayConst>()) {
        if (!(w["enabled"] | false)) continue;
        WeatherState s; s.id=w["id"].as<String>();
        s.sourceRevision=RemoteResourceModel::sourceRevision(w).c_str();
        weather_.push_back(std::move(s));
    }
}

void RemoteResourceManager::redact(JsonObject config) {
    ProtocolSecret::redactRemoteConfig(config);
}

bool RemoteResourceManager::beginProtocolConfigTransaction(uint32_t timeoutMs) {
    if (!mutex_ ||
        xSemaphoreTakeRecursive(mutex_, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
        return false;
    }
    if (activeExternalCommits_) {
        xSemaphoreGiveRecursive(mutex_);
        return false;
    }
    return true;
}

void RemoteResourceManager::endProtocolConfigTransaction() {
    if (mutex_) xSemaphoreGiveRecursive(mutex_);
}

#if FASTBEE_ENABLE_PERIPH_EXEC
bool RemoteResourceManager::beginObservationCommit(
    bool weatherSource, const String& id, const String& sourceRevision,
    uint32_t generation) {
    Guard lock(mutex_);
    if (generation != generation_) return false;
    if (!weatherSource) return false;
    const char* collection = "weatherSources";
    for (JsonObjectConst source : config_[collection].as<JsonArrayConst>()) {
        if (!(source["enabled"] | false) || id != (source["id"] | "")) continue;
        if (sourceRevision != RemoteResourceModel::sourceRevision(source).c_str()) {
            return false;
        }
        ++activeExternalCommits_;
        return true;
    }
    return false;
}
#endif

void RemoteResourceManager::endExternalCommit() {
    Guard lock(mutex_);
    if (activeExternalCommits_) --activeExternalCommits_;
}

void RemoteResourceManager::getConfig(JsonObject out) {
    Guard lock(mutex_);
    out["remoteChannels"]=config_["remoteChannels"];
    out["weatherSources"]=config_["weatherSources"];
    if (!out["remoteChannels"].is<JsonArray>()) out["remoteChannels"].to<JsonArray>();
    if (!out["weatherSources"].is<JsonArray>()) out["weatherSources"].to<JsonArray>();
    redact(out);
    out["capabilities"]["maxRemoteChannels"]=0;
    out["capabilities"]["maxWeatherSources"]=1;
    out["capabilities"]["maxForecastDays"]=0;
    out["capabilities"]["weatherTestRead"]=true;
    out["capabilities"]["weatherTestMinIntervalSeconds"]=300;
}

bool RemoteResourceManager::saveConfig(JsonVariantConst submitted,String& error) {
    if (!mutex_) { error="Remote resources not initialized"; return false; }
    Guard lock(mutex_);
    if (activeExternalCommits_) {
        error="Remote state is being committed; retry shortly";
        return false;
    }
    if (!submitted["expectedWeatherRevision"].is<uint32_t>() || submitted["expectedWeatherRevision"].as<uint32_t>() != weatherRevision_) { error="Use screen weather settings"; return false; }
    if (submitted["remoteChannels"].size() || submitted["weatherSources"].size() != 1) { error="Remote channels are retired; one weather source is supported"; return false; }
    JsonDocument next; next.set(submitted);
    next.remove("expectedWeatherRevision");
    std::string validation;
    if (!RemoteResourceModel::validate(next.as<JsonVariantConst>(),validation)) { error=validation.c_str(); return false; }
    JsonDocument all;
    if (!ConfigStorage::getInstance().loadJSONConfig("/config/protocol.json",all)) { error="Cannot load existing protocol configuration"; return false; }
    if ((all["screenWeatherRevision"] | 1U) != weatherRevision_) { error="Weather revision conflict; refresh configuration"; return false; }
    JsonObject migration = all["screenWeatherMigration"].to<JsonObject>();
    migration.set(weatherMigration_.as<JsonVariantConst>());
    migration["required"] = false; migration.remove("candidates");
    all["screenWeatherVersion"] = 1; all["screenWeatherRevision"] = weatherRevision_ + 1;
    all["remoteChannels"]=next["remoteChannels"];
    all["weatherSources"]=next["weatherSources"];
    if (!ConfigStorage::getInstance().saveJSONConfig("/config/protocol.json",all)) { error="Cannot save remote resources"; return false; }
    ++weatherRevision_;
    applyConfig(next.as<JsonVariantConst>());
    error=""; return true;
}

void RemoteResourceManager::appendChoices(JsonArray sources,JsonArray controls) {
    Guard lock(mutex_);
    (void)controls;
    for (JsonObjectConst w:config_["weatherSources"].as<JsonArrayConst>()) {
        if (!(w["enabled"] | false)) continue;
        String ref=String("weather.")+(w["id"] | "");
        JsonObject group=sources.add<JsonObject>(); group["id"]=ref; group["sourceRef"]=ref; group["label"]=w["label"]; group["type"]="weather"; group["group"]="weather";
        for (auto field:{"temperature"}) {
            JsonObject s=sources.add<JsonObject>(); s["id"]=ref+"."+field; s["sourceRef"]=ref+"."+field;
            s["label"]=String(w["label"] | "")+" "+field; s["type"]="number"; s["unit"]=!strcmp(field,"temperature") ? "°C":"%"; s["group"]="weather";
        }
    }
}

bool RemoteResourceManager::getValue(const String& ref,JsonObject out) {
    Guard lock(mutex_);
    const uint32_t now=millis();
    for (auto& s:weather_) {
        String base=String("weather.")+s.id;
        if (ref!=base && !ref.startsWith(base+".")) continue;
        JsonObjectConst cfg;
        for (JsonObjectConst w:config_["weatherSources"].as<JsonArrayConst>()) if (s.id==w["id"].as<String>()) { cfg=w; break; }
        out["sourceRef"]=ref; out["updatedMs"]=s.updatedMs; out["error"]=s.error;
        out["quality"]=!s.known ? "unknown" : now-s.updatedMs>(cfg["staleAfterSeconds"] | 7200U)*1000U ? "stale" : "good";
        if (ref==base) {
            out["weather"]=s.value.as<JsonVariantConst>();
        } else {
            String fieldName=ref.substring(base.length()+1);
            if (fieldName!="temperature" && fieldName!="humidity") return false;
            out["value"]=s.value[fieldName]; out["unit"]=fieldName=="temperature" ? "°C":"%";
            if (out["value"].isNull()) out["quality"]="unknown";
        }
        return true;
    }
    return false;
}

bool RemoteResourceManager::execute(const String& id,bool target,String& status,String& error) {
    (void)id; (void)target; status="retired"; error="Remote channels are retired"; return false;
}
bool RemoteResourceManager::execute(const String& id,const String& numericValue,String& status,String& error) {
    (void)id; (void)numericValue; status="retired"; error="Remote channels are retired"; return false;
}
bool RemoteResourceManager::consumeMqtt(const String& topic,const String& payload) {
    (void)topic; (void)payload; return false;
}
void RemoteResourceManager::handleMqtt(MQTTClient* client) {
#if FASTBEE_ENABLE_MQTT
    if (!client || !client->getIsConnected()) return;
    String topic;
    { Guard lock(mutex_); if (!retiredTopics_.empty()) topic=retiredTopics_.back(); }
    // Only retire subscriptions created by the removed dedicated channel feature.
    if (topic.length() && client->unsubscribeExact(topic)) {
        Guard lock(mutex_);
        retiredTopics_.erase(std::remove(retiredTopics_.begin(),retiredTopics_.end(),topic),retiredTopics_.end());
    }
#else
    (void)client;
#endif
}

bool RemoteResourceManager::requestRead(const String& id,String& error) {
    Guard lock(mutex_);
    for (auto& s:weather_) if (s.id==id || String("weather.")+s.id==id) {
        const uint32_t now=millis();
        if ((s.updatedMs && now-s.updatedMs<300000U) ||
            (s.manualRequestSeen && now-s.lastManualRequestMs<300000U)) {
            error="Weather minimum refresh interval is 5 minutes";
            return false;
        }
        s.requested=true;
        s.manualRequestSeen=true;
        s.manualRequestPending=true;
        s.lastManualRequestMs=now;
        error="";
        return true;
    }
    error="Weather source unavailable"; return false;
}

bool RemoteResourceManager::getWeatherTestStatus(const String& id,JsonObject out) {
    Guard lock(mutex_);
    const uint32_t now=millis();
    for (auto& s:weather_) if (s.id==id || String("weather.")+s.id==id) {
        JsonObjectConst source;
        for (JsonObjectConst candidate:config_["weatherSources"].as<JsonArrayConst>()) {
            if (s.id==candidate["id"].as<String>()) { source=candidate; break; }
        }
        out["sourceId"]=s.id;
        out["label"]=source["label"] | "";
        out["status"]=s.manualRequestPending ? "pending" :
            (s.error.length() ? "error" : (s.known ? "ready" : "idle"));
        out["pending"]=s.manualRequestPending;
        out["updatedMs"]=s.updatedMs;
        uint32_t retryAfterMs=0;
        if (s.error.length()) {
            // Transient errors (NTP, network, memory) should retry soon
            retryAfterMs = 30000U; // 30 seconds
        } else {
            for (uint32_t stamp:{s.updatedMs,s.manualRequestSeen ? s.lastManualRequestMs : 0U}) {
                if (!stamp) continue;
                const uint32_t elapsed=now-stamp;
                if (elapsed<300000U) retryAfterMs=std::max(retryAfterMs,300000U-elapsed);
            }
        }
        out["retryAfterMs"]=retryAfterMs;
        if (s.error.length()) out["error"]=s.error;
        RemoteResourceModel::copyWeatherPreview(s.value.as<JsonVariantConst>(),out["preview"].to<JsonObject>());
        return true;
    }
    return false;
}

void RemoteResourceManager::tick() {
    if (!mutex_) return;
    // Keep a due source due while offline. A disconnect after this admission
    // check is handled by the bounded transient retry in collectWeather().
    const bool networkConnected=weatherNetworkConnected();
    Guard lock(mutex_);
    uint32_t now=millis(); bool ready=false;
    for (auto& s:weather_) {
        for (JsonObjectConst w:config_["weatherSources"].as<JsonArrayConst>()) if (s.id==w["id"].as<String>()) {
            if (s.requested || due(now,s.nextMs)) ready=true;
        }
    }
    const size_t internalFree=ready
        ? heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT) : 0U;
    if (!weatherWorkerEligible(networkConnected,workerActive_,!weather_.empty(),
                               otaBusy(),ready,internalFree)) return;
    workerActive_=true;
    if (xTaskCreate(weatherTask,"weather_read",8192,this,0,nullptr)!=pdPASS) workerActive_=false;
}

void RemoteResourceManager::weatherTask(void* arg) {
    auto* self=static_cast<RemoteResourceManager*>(arg);
    self->collectWeather();
    { Guard lock(self->mutex_); self->workerActive_=false; }
    vTaskDelete(nullptr);
}

void RemoteResourceManager::collectWeather() {
    JsonDocument source; String id; bool manual=false; uint32_t generation=0;
    {
        Guard lock(mutex_); const uint32_t now=millis();
        // A user-triggered test must not wait behind an unrelated periodic
        // source (each bounded HTTP attempt can take several seconds).
        for (unsigned pass=0;pass<2 && !id.length();++pass) {
            const bool manualOnly=pass==0;
            for (auto& s:weather_) {
                if (manualOnly!=s.requested) continue;
                for (JsonObjectConst w:config_["weatherSources"].as<JsonArrayConst>()) if (s.id==w["id"].as<String>()) {
                    bool current=s.requested || due(now,s.nextMs);
                    if (current) {
                        source.set(w); id=s.id; manual=s.requested;
                        generation=generation_; s.requested=false; break;
                    }
                }
                if (id.length()) break;
            }
        }
    }
    if (!id.length()) return;
    JsonDocument parsed; String error;
    bool ok=fetchWeather(source.as<JsonObjectConst>(),parsed,error);
    bool observed=false; uint32_t sampledAt=0;
    {
    Guard lock(mutex_);
    if (generation!=generation_) return;
    for (auto& s:weather_) if (s.id==id) {
        uint32_t now=millis();
        uint32_t interval=(source["intervalSeconds"] | 1800U)*1000U;
        if (ok) {
            s.value.set(parsed.as<JsonVariantConst>());
            s.updatedMs=now; s.known=true; s.error="";
            observed=true; sampledAt=now;
            s.failures=0;
        } else {
            s.error=error;
            s.failures=std::min<uint8_t>(s.failures+1,6);
            const bool transientTransportFailure=
                error=="Network offline" || error=="Weather network request failed";
            interval=weatherFailureDelayMs(s.failures,transientTransportFailure,
                                           interval);
        }
        s.nextMs=now+interval;
        if (manual) s.manualRequestPending=false;
        break;
    }
    }
#if FASTBEE_ENABLE_PERIPH_EXEC
    const String sourceRevision=
        RemoteResourceModel::sourceRevision(source.as<JsonObjectConst>()).c_str();
    if (observed && beginObservationCommit(
            true,id,sourceRevision,generation)) {
        for (auto field:{"temperature"}) {
            if (parsed[field].isNull()) continue;
            ObservationUpdate update;
            update.sourceRef=String("weather.")+id+"."+field;
            update.label=String(source["label"] | "")+" "+field;
            update.value=String(parsed[field].as<double>(),3);
            update.unit=!strcmp(field,"temperature") ? "°C":"%";
            update.sourceRevision=sourceRevision;
            update.maxAgeMs=(source["staleAfterSeconds"] | 7200U)*1000U;
            update.sampleTimestamp=sampledAt; update.recordable=true;
            PeriphExecManager::getInstance().upsertObservation(update);
        }
        endExternalCommit();
    }
#endif
}
} // namespace FastBee
#endif
