"""Real screen save/envelope methods with filesystem and lock fault injection."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cassert>
#include "utils/CheckedText.h"
#include "core/ScreenOutputIdentity.h"
inline void convertToJson(const String& text, JsonVariant out) { out.set(std::string(text)); }
inline void convertFromJson(JsonVariantConst in, String& out) { out=in.as<std::string>(); }
static bool busy=false,mutexFails=false;static std::string scenario;
void* xSemaphoreCreateMutex(){return mutexFails?nullptr:reinterpret_cast<void*>(1);}
struct ScreenPanelMigration { static bool upgrade(JsonObject){return true;} };
bool backupPanelBeforeTouchMigration(const String&){return false;}
struct PanelLock { explicit PanelLock(void*){} bool locked()const{return !busy;} };
struct ScreenPanelManager {
 enum class SaveResult {SAVED,STORAGE_ERROR,INVALID_CONFIG,BUSY,REVISION_CONFLICT};
 static constexpr size_t MAX_CONFIG_BYTES=20*1024;
 static constexpr const char* CONFIG_FILE="/config/screen_panel.json";
 bool _initialized=true,_persisted=true,_applied=true,_restartRequired=false;
 void* _mutex=nullptr; uint32_t _revision=7;
 String _configJson="old",_displayRef="old-display",_activePageId="old-page",_applicationError;
 bool initialize();
 bool loadConfigFile(const String&,String& text,uint32_t& revision,bool* migration)const {
  if(scenario=="init_missing")return false;
  text=scenario=="init_parse_fail"?"{":"{\"displayRef\":\"display\",\"settings\":{\"homePageId\":\"home\"}}";
  revision=scenario=="init_max"?UINT32_MAX:7;*migration=scenario=="init_migration_failed";return true;
 }
 static void makeDefaultConfig(JsonDocument& out){out["settings"]["homePageId"]="home";}
 bool validateConfig(JsonVariantConst in,JsonDocument& out,String&){return out.set(in);}
 bool validateAndBindReferences(JsonDocument&,String&){return true;}
 SaveResult saveConfig(JsonVariantConst,uint32_t,String&);
 bool writeEnvelope(JsonVariantConst,uint32_t)const;
};
METHODS
int main(int argc,char**argv){
 assert(argc==2);const std::string mode=argv[1];
 scenario=mode;
 ScreenPanelManager manager;String error;JsonDocument config;
 if(mode.rfind("init_",0)==0){
  manager._initialized=false;FastBee::ScreenOutputIdentity::publish(0);
  mutexFails=mode=="init_lock_fail";
  const bool success=manager.initialize();
  const bool expected=mode!="init_lock_fail" && mode!="init_parse_fail";
  assert(success==expected && manager._initialized==expected);
  const uint32_t published=(mode=="init_existing" || mode=="init_migration_failed")?7:0;
  assert(FastBee::ScreenOutputIdentity::current()==published);
  if(mode=="init_migration_failed")assert(manager._revision==7 && !manager._applicationError.isEmpty());
  if(mode=="init_max")assert(manager._revision==UINT32_MAX);
  return 0;
 }
 FastBee::ScreenOutputIdentity::publish(7);
 assert(!deserializeJson(config,R"({"schemaVersion":1,"displayRef":"display","settings":{"homePageId":"home"},"pages":[{"id":"home"}],"label":"sensor.temp"})"));
 const std::string path=ScreenPanelManager::CONFIG_FILE;
 LittleFS.files[path]="OLD";
 if(mode=="busy")busy=true;
 if(mode=="exhausted")manager._revision=UINT32_MAX;
 if(mode=="open")LittleFS.failOpenWrite=path+".tmp";
 if(mode=="short")LittleFS.writeLimit=8;
 if(mode=="corrupt")LittleFS.mutateJsonOnClose=true;
 if(mode=="rename" || mode=="backup"){
  LittleFS.failRenameFrom=path+".tmp";LittleFS.failRenameTo=path;
 }
 if(mode=="backup"){
  LittleFS.files.erase(path);LittleFS.files[path+".bak"]="ONLY_COMMITTED";
 }
 const uint32_t expected=mode=="conflict"?6:manager._revision;
 const auto result=manager.saveConfig(config.as<JsonVariantConst>(),expected,error);
 if(mode=="success"){
  assert(result==ScreenPanelManager::SaveResult::SAVED);
  assert(manager._revision==8 && manager._displayRef=="display" && manager._activePageId=="home");
  assert(manager._persisted && !manager._applied);
  assert(FastBee::ScreenOutputIdentity::current()==8);
  assert(!FastBee::ScreenOutputIdentity::matches((uint64_t(7)<<32U)|1U));
  JsonDocument runtime,stored;
  assert(!deserializeJson(runtime,manager._configJson.c_str()));
  assert(!deserializeJson(stored,LittleFS.files.at(path)));
  assert(stored["revision"]==8 && stored["touchUiMigration"]==2);
  assert(stored["config"].as<JsonVariantConst>()==runtime.as<JsonVariantConst>());
 }else{
  assert(result!=ScreenPanelManager::SaveResult::SAVED);
  assert(manager._configJson=="old" && manager._displayRef=="old-display" && manager._activePageId=="old-page");
  assert(manager._revision==(mode=="exhausted"?UINT32_MAX:7));
  assert(FastBee::ScreenOutputIdentity::current()==7);
  if(mode=="backup")assert(LittleFS.files.at(path+".bak")=="ONLY_COMMITTED");
  else assert(LittleFS.files.at(path)=="OLD");
 }
}
'''


class ScreenConfigPersistenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        source = (ROOT / 'src/core/ScreenPanelManager.cpp').read_text(encoding='utf-8')
        save = source[source.index('ScreenPanelManager::SaveResult ScreenPanelManager::saveConfig('):source.index('bool ScreenPanelManager::queueSetting(')]
        write = source[source.index('bool ScreenPanelManager::writeEnvelope('):source.index('void ScreenPanelManager::appendCapabilities(')]
        initialize = source[source.index('bool ScreenPanelManager::initialize()'):source.index('bool ScreenPanelManager::loadConfigFile(')]
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-screen-config-')
        path = Path(cls.temp.name) / 'harness.cpp'
        path.write_text(HARNESS.replace('METHODS', initialize + '\n' + save + '\n' + write), encoding='utf-8')
        cls.binary = path.with_suffix('.exe' if os.name == 'nt' else '')
        includes = ['test/unit/fixtures/point_history', 'test/mocks',
                    '.pio/libdeps/esp32s3-F16R8/ArduinoJson/src', 'include']
        result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra',
                                 '-DFASTBEE_ENABLE_SCREEN_UI=0', '-DARDUINOJSON_ENABLE_ARDUINO_STRING=0',
                                 '-DARDUINOJSON_ENABLE_STD_STRING=1',
                                 *['-I' + str(ROOT / item) for item in includes],
                                 str(path), '-o', str(cls.binary)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            cls.temp.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_durable_save_and_preservation(self):
        for mode in ('success', 'busy', 'conflict', 'exhausted', 'open', 'short', 'corrupt', 'rename', 'backup',
                     'init_lock_fail', 'init_parse_fail', 'init_missing', 'init_existing', 'init_max', 'init_migration_failed'):
            with self.subTest(mode=mode):
                result = subprocess.run([str(self.binary), mode], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__':
    unittest.main()
