"""Exercise bounded cold-start recovery using the production device owner."""
from pathlib import Path
import unittest
from test.unit import test_panel_runtime_regressions as helpers

ROOT = Path(__file__).resolve().parents[2]


class ScreenStartupRecoveryTests(unittest.TestCase):
    compile_run = helpers.PanelRuntimeRegressionTests.compile_run

    def test_shared_reset_retries_once_before_ui_activation(self):
        source = (ROOT / 'src/screen/ScreenDevice.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(signature.replace('ScreenDevice::', '') + helpers.block(source, signature)
                            for signature in ('bool ScreenDevice::activate(String& error)',
                                              'bool ScreenDevice::deactivate(String* error)',
                                              'bool ScreenDevice::initializeTouch()',
                                              'bool ScreenDevice::serviceTouch(uint32_t nowMs)'))
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <cstdio>
#include <cstdlib>
#undef assert
#define assert(c) do {if(!(c)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#c);std::exit(86);}}while(false)
#define FASTBEE_ENABLE_SCREEN_UI 1
#define LOG_INFOF(...) do {} while(false)
#define LOG_WARNING(...) do {} while(false)
#define LOG_WARNINGF(...) do {} while(false)
class String: public std::string {public: using std::string::string;using std::string::operator=;
  String(const std::string& s):std::string(s){} bool isEmpty()const{return empty();}};
uint32_t now=0;uint32_t millis(){return now;}
struct Config {char backend[8]="rgb",driver[8]="st7701",touchBackend[8]="gt911";
  int touchRst=-1;uint8_t controlResetMask=10,controlTouchIntMask=4,brightness=52;
  unsigned width=480,height=480;};
unsigned displayBegins=0,displayEnds=0,touchBegins=0,touchEnds=0;
bool displayFails=false,stopFails=false,resetRecovers=true;std::string touchFailure="SCREEN_TOUCH_GT911_CONFIG_INVALID";
struct Display {
  bool live=false;
  bool begin(const Config&,String& error){++displayBegins;now+=500;
    if(displayFails&&displayBegins==2){error="DISPLAY_RESTART_FAILED";return false;}live=true;return true;}
  void end(){live=false;++displayEnds;}
  bool stop(String& error){if(stopFails){error="DISPLAY_STOP_FAILED";return false;}
    end();error="";return true;}
  bool setBrightness(uint8_t value){assert(live&&value==52);return true;}
};
struct Touch {
  bool live=false;
  bool begin(const Config&,String& error){++touchBegins;
    if(displayBegins<2||!resetRecovers){error=touchFailure;return false;}live=true;return true;}
  bool ready(){return live;}
  void end(){++touchEnds;live=false;}
};
std::unique_ptr<Display> createDisplayBackend(const Config&,String&){return std::unique_ptr<Display>(new Display);}
std::unique_ptr<Touch> createTouchBackend(const Config&,String&){return std::unique_ptr<Touch>(new Touch);}
struct Device {
  bool _active=false,_configured=true,_stopping=false;
  Config _config;String _peripheralId="panel",_touchError;
  uint32_t _lastTouchAttemptMs=0;
  std::unique_ptr<Display> _display;std::unique_ptr<Touch> _touch;
  METHODS
};
void reset(){displayBegins=displayEnds=touchBegins=touchEnds=0;now=0;
  displayFails=stopFails=false;resetRecovers=true;touchFailure="SCREEN_TOUCH_GT911_CONFIG_INVALID";}
int main(){
  for(const char* errorCode:{"SCREEN_TOUCH_GT911_CONFIG_INVALID","SCREEN_TOUCH_GT911_CONFIG_CHECKSUM_INVALID"}){
    reset();touchFailure=errorCode;Device d;String error;
    assert(d.activate(error)&&error.empty()&&d._active&&d._touch&&d._touch->ready());
    assert(displayBegins==2&&displayEnds==1&&touchBegins==2&&touchEnds==1);
    assert(d.activate(error)&&displayBegins==2); // no restart of a live UI
    assert(!d.serviceTouch(now+5000)&&displayBegins==2);
  }
  reset();resetRecovers=false;Device failed;String error;
  assert(failed.activate(error)&&failed._active&&!failed._touch);
  assert(displayBegins==2&&failed._touchError=="SCREEN_TOUCH_GT911_CONFIG_INVALID");
  failed.serviceTouch(now+6000);failed.serviceTouch(now+12000);
  assert(displayBegins==2); // permanent faults cannot trigger a reset/flicker loop
  reset();displayFails=true;Device restartFailed;
  assert(!restartFailed.activate(error)&&!restartFailed._active&&error=="DISPLAY_RESTART_FAILED");
  assert(!restartFailed._display&&!restartFailed._touch);
  reset();stopFails=true;Device retained;
  assert(!retained.activate(error)&&error=="DISPLAY_STOP_FAILED");
  const auto* owner=retained._display.get();
  assert(owner&&retained._stopping&&!retained._active&&displayBegins==1&&displayEnds==0);
  assert(!retained.activate(error)&&error.find("SCREEN_DEVICE_CLEANUP_PENDING")==0);
  assert(retained._display.get()==owner&&displayBegins==1);
  assert(!retained.deactivate(&error)&&error=="DISPLAY_STOP_FAILED"&&retained._display.get()==owner);
  stopFails=false;assert(retained.deactivate(&error)&&error.empty());
  assert(!retained._display&&!retained._touch&&!retained._stopping);
  assert(retained.activate(error)&&retained._active&&displayBegins==2);
  for(unsigned variant=0;variant<5;++variant){
    reset();Device d;
    if(variant==0)d._config.touchRst=3; // direct GPIO backend owns its reset
    if(variant==1)d._config.controlTouchIntMask=0;
    if(variant==2)d._config.controlResetMask=0;
    if(variant==3)std::strcpy(d._config.backend,"spi");
    if(variant==4)touchFailure="SCREEN_TOUCH_GT911_NOT_FOUND";
    assert(d.activate(error)&&displayBegins==1&&!d._touch);
  }
}
'''.replace('METHODS', methods), [])


if __name__ == '__main__':
    unittest.main()
