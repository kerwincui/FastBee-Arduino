"""Compile production ADC calibration and conversion with deterministic SDK results."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_checked_adc import mock_sdk

ROOT = Path(__file__).resolve().parents[2]


class CalibratedVoltageTests(unittest.TestCase):
    def test_calibrated_same_sample_conversion_and_failures(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        config = (ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf8')
        enums = config[config.index('enum class AnalogSignalMode'):config.index('// DI 输入模式')]
        start = config.index('inline float positiveAnalogConfigValue')
        conversion = config[start:config.index('// 外设运行时状态', start)]
        harness = r'''
#include <cmath>
#include <cstdint>
#include <cstdio>
#include "esp_adc/adc_cali.h"
#include "core/AnalogInputCalibration.h"
using std::isnan; using std::isinf;
ENUMS
struct PeripheralConfig {
 struct Params { struct Adc {
 uint8_t signalMode=3,resolution=12,attenuation=3;
 bool conversionEnabled=true,clampEnabled=false,calibratedVoltage=false;
 float inputMin=4,inputMax=20,engineeringMin=-40,engineeringMax=80,engineeringOffset=0;
 float vRef=3.3f,voltageRatio=1,shuntResistance=150,inputGain=1,inputOffset=0;
 uint16_t rawMin=0,rawMax=4095;
 } adc; } params;
};
CONVERSION
static int creates=0,deletes=0,lastRaw=-1,nextMv=1886;
static bool failCreate=false,failConvert=false;
bool adcTestMutexFails=false;
bool adcTestForeignPin=false;
unsigned adcTestMutexCreates=0;
int adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t*, UnitHandle**) {return ESP_ERR_NOT_SUPPORTED;}
int adc_oneshot_config_channel(UnitHandle*,int,const adc_oneshot_chan_cfg_t*) {return ESP_ERR_NOT_SUPPORTED;}
int adc_oneshot_read(UnitHandle*,int,int*) {return ESP_ERR_NOT_SUPPORTED;}
int adc_oneshot_del_unit(UnitHandle*) {return ESP_ERR_NOT_SUPPORTED;}
int adc_oneshot_io_to_channel(int pin,int* unit,int* channel) {
 if(pin>19)return 1; *unit=pin/10; *channel=pin%10; return 0;
}
int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t* c,CalibrationHandle** h) {
 if(failCreate)return 1;
 if(c->bitwidth!=12)return 1;
 ++creates;*h=new CalibrationHandle{c->unit_id,c->chan,c->atten};return 0;
}
int adc_cali_delete_scheme_curve_fitting(CalibrationHandle* h) {++deletes;delete h;return 0;}
int adc_cali_raw_to_voltage(CalibrationHandle*,int raw,int* mv) {lastRaw=raw;if(failConvert)return 1;*mv=nextMv;return 0;}
#define CHECK(x) do {if(!(x)){std::printf("Failure line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main() {
 float v=0;
 CHECK(FastBee::calibratedAdcVoltage(7,3,12,2238,v));
 CHECK(lastRaw==2238 && std::fabs(v-1.886f)<0.0001f && creates==1);
 CHECK(FastBee::calibratedAdcVoltage(7,3,12,2240,v) && creates==1);
 CHECK(FastBee::calibratedAdcVoltage(7,3,10,559,v) && lastRaw==2236);
 PeripheralConfig c; AnalogInputQuality q;
 c.params.adc.calibratedVoltage=true;
 CHECK(std::fabs(calculateAnalogInputCurrentMa(c,1.864f)-12.426666f)<0.0001f);
 CHECK(std::fabs(calculateAnalogInputCurrentMa(c,.765f)-5.1f)<0.0001f);
 CHECK(std::fabs(calculateAnalogInputCurrentMa(c,9.18f)-61.2f)<0.001f);
 CHECK(isnan(calculateAnalogInputCurrentMa(c,NAN)));
 CHECK(isnan(calculateAnalogInputCurrentMa(c,INFINITY)));
 CHECK(isnan(calculateAnalogInputCurrentMa(c,-1)));
 c.params.adc.signalMode=1;CHECK(isnan(calculateAnalogInputCurrentMa(c,1)));
 c.params.adc.signalMode=3;
 CHECK(std::fabs(convertAnalogInputRaw(c,2238,&q,1.886f)-24.3f)<0.001f);
 CHECK(q==AnalogInputQuality::VALID);
 CHECK(std::fabs(convertAnalogInputRaw(c,690,&q,.6f)+40)<0.001f);
 CHECK(std::fabs(convertAnalogInputRaw(c,4090,&q,3.f)-80)<0.001f);
 c.params.adc.engineeringMin=0;c.params.adc.engineeringMax=100;
 CHECK(std::fabs(convertAnalogInputRaw(c,690,&q,.6f))<0.001f && q==AnalogInputQuality::VALID);
 CHECK(isnan(convertAnalogInputRaw(c,690,&q,.55f)) && q==AnalogInputQuality::BELOW_RANGE);
 CHECK(isnan(convertAnalogInputRaw(c,0,&q,0)) && q==AnalogInputQuality::DISCONNECTED);
 CHECK(isnan(convertAnalogInputRaw(c,4095,&q,3.3f)) && q==AnalogInputQuality::SHORT_CIRCUIT);
 CHECK(isnan(convertAnalogInputRaw(c,2238,&q)) && q==AnalogInputQuality::INVALID_CONFIG);
 CHECK(isnan(convertAnalogInputRaw(c,2238,&q,INFINITY)));
 CHECK(isnan(convertAnalogInputRaw(c,2238,&q,-1)));
 c.params.adc.calibratedVoltage=false;c.params.adc.shuntResistance=165;
 c.params.adc.engineeringMin=-40;c.params.adc.engineeringMax=80;
 CHECK(std::fabs(convertAnalogInputRaw(c,2238,&q)-11.97802f)<0.001f);
 c.params.adc.conversionEnabled=false;c.params.adc.calibratedVoltage=true;
 CHECK(convertAnalogInputRaw(c,123,&q)==123);
 failConvert=true;CHECK(!FastBee::calibratedAdcVoltage(7,3,12,2238,v) && isnan(v));failConvert=false;
 CHECK(!FastBee::calibratedAdcVoltage(99,3,12,2238,v));
 CHECK(!FastBee::calibratedAdcVoltage(7,4,12,2238,v));
 CHECK(!FastBee::calibratedAdcVoltage(7,3,9,512,v));
 failCreate=true;CHECK(!FastBee::calibratedAdcVoltage(8,3,12,2238,v) && isnan(v));failCreate=false;
 for(uint8_t pin=0;pin<20;++pin) CHECK(FastBee::calibratedAdcVoltage(pin,3,12,2000,v));
 CHECK(creates-deletes==8);
 return 0;
}
'''.replace('ENUMS', enums).replace('CONVERSION', conversion)
        with tempfile.TemporaryDirectory(prefix='fastbee-adc-calibration-') as directory:
            temp = Path(directory)
            mock_sdk(temp)
            (temp/'test.cpp').write_text(harness,encoding='utf8')
            binary=temp/('test.exe' if os.name=='nt' else 'test')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-DSOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED=1', '-I'+str(temp), '-I'+str(ROOT/'test/unit/cpp'),
                '-I'+str(ROOT/'include'),str(temp/'test.cpp'), str(ROOT/'src/core/CheckedAdc.cpp'),
                str(ROOT/'src/core/AnalogInputCalibration.cpp'),'-o',str(binary)],capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_opt_in_persists_and_uses_same_sample_everywhere(self):
        persistence=(ROOT/'src/core/PeripheralManagerPersistence.cpp').read_text(encoding='utf8')
        self.assertIn('params["calibratedVoltage"] | false',persistence)
        self.assertIn('FastBee::setFixedJsonField(params, "calibratedVoltage", config.params.adc.calibratedVoltage)',persistence)
        route=(ROOT/'src/network/handlers/PeripheralReadRoutes.cpp').read_text(encoding='utf8')
        self.assertIn('pm.readAnalogSample(id, sample)',route)
        self.assertIn('FastBee::setFixedJsonField(data, "pinVoltageMv", pinVoltage * 1000.0f)',route)
        self.assertIn('FastBee::setFixedJsonField(data, "adcPin", sample.pin)', route)
        self.assertIn('data["pinVoltageSource"]', route)
        self.assertIn('sample.calculatedCurrentMa', route)
        self.assertIn('FastBee::setFixedJsonField(data, "calculatedCurrentMa", nullptr)', route)
        manager=(ROOT/'src/core/PeripheralManagerAnalog.cpp').read_text(encoding='utf8')
        self.assertIn('sample.value = convertAnalogRaw(*config, sample.raw',manager)
        self.assertNotIn('analogRead(', manager)


if __name__=='__main__':unittest.main()
