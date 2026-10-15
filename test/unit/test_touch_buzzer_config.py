"""Run real display request/save/load code for bounded touch-feedback duration."""
from pathlib import Path
import unittest
from test.unit import test_panel_runtime_regressions as helpers
from test.unit import test_generic_peripheral_route_contract as route_helpers

ROOT = Path(__file__).resolve().parents[2]


class TouchBuzzerConfigTests(unittest.TestCase):
    compile_run = helpers.PanelRuntimeRegressionTests.compile_run

    def test_display_duration_roundtrip_default_and_rejected_inputs(self):
        persistence = (ROOT / 'src/core/PeripheralManagerPersistence.cpp').read_text(encoding='utf-8')
        marker = 'else if (config.type == PeripheralType::SCREEN_DISPLAY)'
        save_at = persistence.index(marker)
        load_at = persistence.rindex(marker, 0, persistence.index('const char* backend = params["backend"]'))
        save = 'void saveDisplay(JsonObject params,const PeripheralConfig& config)' + helpers.block(persistence[save_at:], marker)
        load = 'void loadDisplay(JsonObject params,PeripheralConfig& config,String& parseError)' + helpers.block(persistence[load_at:], marker)
        definitions = '\n'.join(route_helpers.definition(name) for name in (
            'readDisplayRequestInteger',
            'applyDisplayRequestParameters', 'addDisplayParams'))
        json = next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h')).parent
        self.compile_run(r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include "network/FixedJsonField.cpp"
#include <math.h>
#include "core/PeripheralConfig.h"
#include <cassert>
#include <map>
#include <cstring>
constexpr int CHIP_MAX_PWM_CH=8;
size_t strlcpy(char* to,const char* from,size_t n){if(n)snprintf(to,n,"%s",from);return strlen(from);}
struct AsyncWebServerRequest{std::map<std::string,String> fields;bool hasParam(const char* n,bool=false){return fields.count(n);}};
#include "core/PeripheralFormFields.h"
struct HarnessFormFields final : public FastBee::PeripheralFormFields {
 explicit HarnessFormFields(AsyncWebServerRequest* r):request(r){}
 bool has(const char* n) const override{return request->hasParam(n,true)||request->hasParam(n);}
 String value(const char* n,const char* fallback) const override{
  auto found=request->fields.find(n);
  return found==request->fields.end()?String(fallback):found->second;
 }
 int intValue(const char* n,int fallback) const override{
  auto found=request->fields.find(n);
  return found==request->fields.end()?fallback:found->second.toInt();
 }
 bool boolValue(const char* n,bool fallback) const override{
  auto found=request->fields.find(n);
  return found==request->fields.end()?fallback
      :(found->second.equalsIgnoreCase("true")||found->second=="1");
 }
private:
 AsyncWebServerRequest* request;
};
using FastBee::PeripheralFormFields;
DEFINITIONS
SAVE
LOAD
int main(){
 PeripheralConfig c;c.type=PeripheralType::SCREEN_DISPLAY;initializeDisplayDefaults(c);
 c.params.display.rgbFrequency=12000000;c.params.display.hsyncPulseWidth=8;c.params.display.vsyncPulseWidth=8;
 assert(c.params.display.touchBuzzerDurationMs==30);
 AsyncWebServerRequest empty;String error;HarnessFormFields emptyForm(&empty);
 assert(applyDisplayRequestParameters(emptyForm,c,false,error));
 JsonDocument stored;saveDisplay(stored.to<JsonObject>(),c);
 stored.remove("touchBuzzerDurationMs");PeripheralConfig legacy;legacy.type=PeripheralType::SCREEN_DISPLAY;initializeDisplayDefaults(legacy);
 loadDisplay(stored.as<JsonObject>(),legacy,error);assert(error.isEmpty()&&legacy.params.display.touchBuzzerDurationMs==30);
 for(unsigned value:{20,30,45,60,100,150}){
  AsyncWebServerRequest request;request.fields["touchBuzzerDurationMs"]=String(value);
  HarnessFormFields form(&request);
  assert(applyDisplayRequestParameters(form,c,false,error));assert(c.params.display.touchBuzzerDurationMs==value);
  JsonDocument response;addDisplayParams(response.to<JsonObject>(),c);assert(response["touchBuzzerDurationMs"]==value);
  stored.clear();saveDisplay(stored.to<JsonObject>(),c);assert(stored["touchBuzzerDurationMs"]==value);
  initializeDisplayDefaults(legacy);error="";loadDisplay(stored.as<JsonObject>(),legacy,error);
  assert(error.isEmpty()&&legacy.params.display.touchBuzzerDurationMs==value);
 }
 for(const char* invalid:{"19","151","-1","256","30.5","30ms","","99999999999999999999999"}){
  auto before=c;AsyncWebServerRequest request;request.fields["touchBuzzerDurationMs"]=invalid;
  HarnessFormFields form(&request);
  assert(!applyDisplayRequestParameters(form,c,false,error));
  assert(memcmp(&before.params.display,&c.params.display,sizeof(c.params.display))==0);
 }
 for(int invalid:{-1,0,19,151,256}){
  stored["touchBuzzerDurationMs"]=invalid;initializeDisplayDefaults(legacy);error="";
  loadDisplay(stored.as<JsonObject>(),legacy,error);assert(!error.isEmpty()&&legacy.params.display.touchBuzzerDurationMs==30);
 }
 stored["touchBuzzerDurationMs"]="60";initializeDisplayDefaults(legacy);error="";
 loadDisplay(stored.as<JsonObject>(),legacy,error);assert(!error.isEmpty());
}
'''.replace('DEFINITIONS', definitions).replace('SAVE', save).replace('LOAD', load),
                         [ROOT / 'test/mocks', ROOT / 'include', ROOT / 'src', json])


if __name__ == '__main__':
    unittest.main()
