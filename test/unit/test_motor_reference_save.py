"""Exercise the production Modbus restart decision with real JSON documents."""
from pathlib import Path
import unittest
from test.unit.test_panel_runtime_regressions import PanelRuntimeRegressionTests

ROOT = Path(__file__).resolve().parents[2]


class MotorReferenceSaveTests(unittest.TestCase):
    compile_run = PanelRuntimeRegressionTests.compile_run

    def test_unchanged_save_retains_runtime_but_changed_bus_restarts(self):
        source = (ROOT / 'src/network/handlers/ProtocolRouteHandler.cpp').read_text(encoding='utf-8')
        start = source.index('const bool modbusChanged =')
        decision = source[start:source.index('#endif', start)]
        self.assertIn('previousModbusRtu.set(doc["modbusRtu"])', source)
        self.compile_run(r'''
#include <ArduinoJson.h>
#include <cassert>
struct ProtocolManager {
 int restarts=0,stops=0;
 bool restartModbusDeferred(){++restarts;return true;}
 void stopModbus(){++stops;}
};
struct Context{ProtocolManager* protocolManager;};
void save(const char* before,const char* after,bool update,int restart,int stop){
 JsonDocument previousModbusRtu,doc;
 assert(!deserializeJson(previousModbusRtu,before));
 JsonDocument input;assert(!deserializeJson(input,after));doc["modbusRtu"].set(input);
 ProtocolManager pm;Context context{&pm};Context* ctx=&context;
 bool updateModbusRtu=update,modbusRestarted=false;
 DECISION
 assert(pm.restarts==restart&&pm.stops==stop&&modbusRestarted==(restart!=0));
}
int main(){
 const char* initial=R"({"enabled":true,"master":{"devices":[{"slaveAddress":8,"motorMinPosition":-6640,"motorMaxPosition":17360}]}})";
 save(initial,initial,true,0,0);
 save(initial,R"({"master":{"devices":[{"motorMaxPosition":17360,"slaveAddress":8,"motorMinPosition":-6640}]},"enabled":true})",true,0,0);
 save(initial,R"({"enabled":true,"master":{"devices":[{"slaveAddress":8,"motorMinPosition":-6640,"motorMaxPosition":17000}]}})",true,1,0);
 save(initial,R"({"enabled":false})",true,0,1);
 save(initial,R"({"enabled":false})",false,0,0);
 save("null",R"({"enabled":true})",true,1,0);
}
'''.replace('DECISION', decision), [ROOT / '.pio/libdeps/esp32s3-F16R8/ArduinoJson/src'])


if __name__ == '__main__':
    unittest.main()
