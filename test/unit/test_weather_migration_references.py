"""Execute real weather-save and rule-reference snapshot methods."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test.unit.test_periph_exec_config_persistence import cpp_function

ROOT=Path(__file__).resolve().parents[2]

class WeatherMigrationReferenceTests(unittest.TestCase):
    def test_migration_rejects_reference_loss_before_any_save(self):
        remote=(ROOT/'src/protocols/RemoteResourceManager.cpp').read_text(encoding='utf-8')
        rules=(ROOT/'src/core/PeriphExecRuleQueries.cpp').read_text(encoding='utf-8')
        manager=(ROOT/'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        header=(ROOT/'include/core/PeriphExecManager.h').read_text(encoding='utf-8')
        production=cpp_function(header,'const char* configurationWriteError()').replace('configurationWriteError()', 'PeriphExecManager::configurationWriteError()')+'\n'+cpp_function(manager,'bool PeriphExecManager::removeRuleLocked(')+'\n'+cpp_function(rules,'bool PeriphExecManager::getWeatherRuleReferences(')+'\n'+cpp_function(rules,'void PeriphExecManager::releaseWeatherRuleReferences(')+'\n'+cpp_function(remote,'bool RemoteResourceManager::saveScreenWeather(')
        program=r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cassert>
#include <new>
#include <functional>
#include <thread>
#include "protocols/RemoteResourceModel.h"
inline void convertFromJson(JsonVariantConst v,String& s) { s=v.as<std::string>(); }
static thread_local bool remoteLocked=false; static bool ruleLockWorks=true;
static std::function<void()> beforeRemoteLock;
struct Log { template<class... T> void infof(const char*,T...){} } LOGGER;
bool configError(String& error,const char* value){error=value;return false;}
struct Guard { Guard(void*) { if(beforeRemoteLock){auto callback=std::move(beforeRemoteLock);beforeRemoteLock={};callback();} remoteLocked=true; } ~Guard() { remoteLocked=false; } };
constexpr unsigned portMAX_DELAY=0xffffffffU;
struct MutexGuard { MutexGuard(int,unsigned=0) { assert(!remoteLocked); } bool isLocked() const { return ruleLockWorks; } };
struct Trigger { String triggerPeriphId,eventId; };
struct Action { String targetPeriphId; };
struct Rule { bool enabled=false; std::vector<Trigger> triggers; std::vector<Action> actions; };
struct PeriphExecManager {
 mutable bool _weatherReferenceLease=false;
 int _rulesMutex=1; bool _configurationLoadFailed=false,_configurationRestartPending=false;
 std::map<String,Rule> rules;
 static PeriphExecManager& getInstance() { static PeriphExecManager m; return m; }
 bool getWeatherRuleReferences(std::vector<String>&,bool=false) const;
 void releaseWeatherRuleReferences();
 const char* configurationWriteError() const;
 bool removeRuleLocked(const String&,String&);
};
struct ConfigStorage {
 JsonDocument legacy;
 static ConfigStorage& getInstance() { static ConfigStorage s; return s; }
 bool loadJSONConfig(const char*,JsonDocument& out) { out.set(legacy); return true; }
};
using namespace FastBee;
struct RemoteResourceManager {
 void* mutex_=reinterpret_cast<void*>(1);
 JsonDocument config_,weatherMigration_;
 uint32_t weatherRevision_=2;
 unsigned saves=0;
 bool saveScreenWeather(JsonVariantConst,String&);
 bool saveConfig(JsonVariantConst submitted,String&) { assert(PeriphExecManager::getInstance()._weatherReferenceLease); ++saves; config_.set(submitted); return true; }
};
PRODUCTION
int main() {
 auto& rules=PeriphExecManager::getInstance();
 auto& storage=ConfigStorage::getInstance();
 auto array=storage.legacy["weatherSources"].to<JsonArray>();
 for (const char* id : {"one","two"}) {
   auto w=array.add<JsonObject>(); w["id"]=id; w["provider"]="seniverse"; w["enabled"]=true; w["apiKey"]="private";
 }
 RemoteResourceManager m; m.weatherMigration_["required"]=true;
 RemoteResourceModel::fixedWeather(m.config_["weatherSources"].to<JsonArray>().add<JsonObject>(),JsonObjectConst());
 JsonDocument submitted; submitted["expectedRevision"]=2; submitted["enabled"]=true;
 submitted["apiKey"]=""; submitted["location"]="beijing"; submitted["intervalSeconds"]=1800; submitted["legacySourceId"]="one";
 String error;
 for (int form=0;form<3;++form) {
   rules.rules.clear(); auto& r=rules.rules["disabled-rule"];
   if (form==0) r.triggers.push_back({"weather.two.temperature",""});
   if (form==1) r.triggers.push_back({"","ds:weather.two.temperature"});
   if (form==2) r.actions.push_back({"weather.two"});
   assert(!m.saveScreenWeather(submitted.as<JsonVariantConst>(),error));
   assert(error=="Weather migration would remove a referenced source");
   assert(m.saves==0 && m.weatherMigration_["required"]==true);
   assert(!rules._weatherReferenceLease);
 }
 // Prefix similarity must not mistake a different source for the retained one.
 rules.rules.clear(); rules.rules["prefix"].triggers.push_back({"weather.oneExtra.temperature",""});
 assert(!m.saveScreenWeather(submitted.as<JsonVariantConst>(),error) && m.saves==0);
 rules.rules.clear(); ruleLockWorks=false;
 assert(!m.saveScreenWeather(submitted.as<JsonVariantConst>(),error));
 assert(error=="Weather rule references unavailable" && m.saves==0);
 ruleLockWorks=true; rules._configurationRestartPending=true;
 assert(!m.saveScreenWeather(submitted.as<JsonVariantConst>(),error) && m.saves==0);
 rules._configurationRestartPending=false;
 rules.rules["keep"].triggers.push_back({"","ds:weather.one.temperature"});
 beforeRemoteLock=[&]{std::thread writer([&]{String issue;assert(!rules.removeRuleLocked("keep",issue));assert(issue=="WEATHER_MIGRATION_BUSY");});writer.join();};
 assert(m.saveScreenWeather(submitted.as<JsonVariantConst>(),error) && m.saves==1);
 assert(m.weatherMigration_["required"]==false && !rules._weatherReferenceLease);
 assert(m.config_["weatherSources"][0]["id"]=="one");
 assert(m.config_["weatherSources"][0]["apiKey"]=="private");
}
'''.replace('PRODUCTION',production)
        compiler=shutil.which('g++') or 'D:/msys64/ucrt64/bin/g++.exe'
        headers=next((ROOT/'.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
        env=dict(os.environ); env['PATH']=str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
        with tempfile.TemporaryDirectory(prefix='fastbee-weather-migration-') as folder:
            temp=Path(folder); cpp=temp/'migration.cpp'; binary=temp/'migration.exe'; cpp.write_text(program,encoding='utf-8')
            result=subprocess.run([compiler,'-std=c++17','-pthread','-Wall','-Wextra','-Werror',
                '-DFASTBEE_ENABLE_PERIPH_EXEC=1','-DARDUINOJSON_ENABLE_ARDUINO_STRING=0','-DARDUINOJSON_ENABLE_STD_STRING=1',
                *[f'-I{p}' for p in (ROOT/'test/mocks',headers.parent,ROOT/'include')],str(cpp),'-o',str(binary)],
                capture_output=True,text=True,encoding='utf-8',errors='replace',timeout=60,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)

if __name__=='__main__': unittest.main()
