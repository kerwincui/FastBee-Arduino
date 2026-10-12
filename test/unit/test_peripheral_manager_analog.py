"""Compile the complete production analog PM translation unit and report helper."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]


class PeripheralManagerAnalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler:
            raise RuntimeError('C++17 compiler required')
        cls.temp = tempfile.TemporaryDirectory(prefix='fastbee-pm-analog-')
        cls.addClassCleanup(cls.temp.cleanup)
        folder = Path(cls.temp.name)
        (folder/'core').mkdir()
        config = (ROOT/'include/core/PeripheralConfig.h').read_text(encoding='utf-8')
        enums = config[config.index('enum class AnalogSignalMode'):config.index('// DI 输入模式')]
        conversion = config[config.index('inline float positiveAnalogConfigValue'):config.index('// 外设运行时状态')]
        sample = definition((ROOT/'include/core/PeripheralManager.h').read_text(encoding='utf-8'), 'struct AnalogReadSample') + ';'
        header = r'''
#pragma once
#include <map>
#include <string>
#include <cstdint>
#include <cmath>
#include <cstdio>
using std::isnan; using std::isinf;
class String: public std::string {
public:
 using std::string::string;
 String()=default;
 String(uint16_t value):std::string(std::to_string(value)){}
 bool isEmpty()const{return empty();}
};
ENUMS
enum class PeripheralType{GPIO_ANALOG_INPUT,ADC,GPIO_DIGITAL_INPUT};
enum class GPIOState{STATE_UNDEFINED,STATE_LOW,STATE_HIGH};
struct PeripheralConfig {
 String id="adc"; uint8_t pin=0; bool enabled=true; PeripheralType type=PeripheralType::ADC;
 uint8_t getPrimaryPin()const{return pin;}
 struct Params{struct Adc{
 uint8_t signalMode=0,resolution=12,attenuation=3,sampleCount=1,filterMode=0,decimalPlaces=2;
 bool conversionEnabled=false,clampEnabled=false,calibratedVoltage=false;
 float inputMin=4,inputMax=20,engineeringMin=-40,engineeringMax=80,engineeringOffset=0;
 float vRef=3.3f,voltageRatio=1,shuntResistance=150,inputGain=1,inputOffset=0;
 uint16_t rawMin=0,rawMax=4095;
 }adc;}params;
};
CONVERSION
SAMPLE
struct Runtime{String lastError;};
class PeripheralManager {
public:
 int _mutex=1;
 bool present=true,reserved=false,pinAvailable=true;
 PeripheralConfig config;
 std::map<String,Runtime> runtimeStates{{"adc",{}}};
 PeripheralConfig* getPeripheralCStrLocked(const char* id){return present && config.id==id?&config:nullptr;}
 String getPinPeripheralId(uint8_t pin){return pin==config.pin?config.id:String();}
 bool isReservedPin(uint8_t)const{return reserved;}
 bool managedPinAvailableForAdc(uint8_t)const{return pinAvailable;}
 void invalidateSensorConfigurationLocked(){}
 bool setupAnalogInput(const PeripheralConfig&);
 bool teardownAnalogInput(const PeripheralConfig&);
 bool readAnalogSample(const String&,AnalogReadSample&);
 bool readAnalogSampleByCStr(const char*,AnalogReadSample&);
 GPIOState readAnalogGpioBranch(const PeripheralConfig& config){ switch(config.type) { ANALOG_BRANCH default:return GPIOState::STATE_UNDEFINED; } }
 uint16_t readAnalog(uint8_t);
 uint16_t readAnalog(const String&);
 uint16_t readAnalogByCStr(const char*);
 float readAnalogValue(const String&);
 float readAnalogValueByCStr(const char*);
 float convertAnalogRawValue(const String&,uint16_t,AnalogInputQuality* = nullptr,float* = nullptr);
};
'''.replace('ENUMS', enums).replace('CONVERSION', conversion).replace('SAMPLE', sample)
        pm = (ROOT/'src/core/PeripheralManager.cpp').read_text(encoding='utf-8')
        read_gpio = definition(pm, 'GPIOState PeripheralManager::readGpioStateLocked(')
        header = header.replace('ANALOG_BRANCH', definition(read_gpio, 'case PeripheralType::GPIO_ANALOG_INPUT: {'))
        (folder/'core/PeripheralManager.h').write_text(header, encoding='utf-8')
        (folder/'core/AsyncExecTypes.h').write_text(r'''
#pragma once
extern bool managerBusy;
extern unsigned managerHeld;
#define pdMS_TO_TICKS(value) (value)
struct RecursiveMutexGuard {
 bool acquired;
 explicit RecursiveMutexGuard(int,unsigned=0):acquired(!managerBusy){if(acquired)++managerHeld;}
 ~RecursiveMutexGuard(){if(acquired)--managerHeld;}
 bool isLocked()const{return acquired;}
};
''', encoding='utf-8')
        protocol = definition((ROOT/'src/protocols/ProtocolManager.cpp').read_text(encoding='utf-8'), 'static String formatAnalogReportValue(')
        code = (ROOT/'test/unit/cpp/peripheral_manager_analog_harness.cpp.in').read_text(encoding='utf-8').replace('// @report@', protocol)
        (folder/'test.cpp').write_text(code, encoding='utf-8')
        cls.binary = folder/'analog.exe'
        result = subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror',
            '-I'+str(folder),'-I'+str(ROOT/'include'),str(folder/'test.cpp'),
            str(ROOT/'src/core/PeripheralManagerAnalog.cpp'),str(ROOT/'src/core/AnalogInputCalibration.cpp'),
            '-o',str(cls.binary)],capture_output=True,text=True,timeout=90)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def check(self, mode):
        result=subprocess.run([str(self.binary),mode],capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)


for _mode in ('zero','failure','filter','calibration','init','unavailable','gpio_resolution'):
    setattr(PeripheralManagerAnalogTests,'test_'+_mode,lambda self,mode=_mode:self.check(mode))

if __name__=='__main__':
    unittest.main()
