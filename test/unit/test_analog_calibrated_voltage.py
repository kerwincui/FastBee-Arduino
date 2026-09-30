"""Compile production ADC calibration and conversion with deterministic SDK results."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CalibratedVoltageTests(unittest.TestCase):
    def test_calibrated_same_sample_conversion_and_failures(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler)
        config = (ROOT / 'include/core/PeripheralConfig.h').read_text(encoding='utf8')
        enums = config[config.index('enum class AnalogSignalMode'):config.index('// DI 输入模式')]
        start = config.index('inline float positiveAnalogConfigValue')
        conversion = config[start:config.index('// 外设运行时状态', start)]
        sdk = r'''
#pragma once
#include <cstdint>
using adc_unit_t = int; using adc_channel_t = int; using adc_atten_t = int;
using adc_cali_handle_t = void*; using esp_err_t = int;
constexpr int ADC_UNIT_1=0, ADC_CHANNEL_0=0, ADC_BITWIDTH_12=12;
constexpr int ESP_OK=0, ESP_ERR_NOT_SUPPORTED=1;
#define ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED 1
#define ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED 0
#define SOC_ADC_CALIB_CHAN_COMPENS_SUPPORTED 1
struct adc_cali_curve_fitting_config_t {int unit_id,chan,atten,bitwidth;};
int adc_oneshot_io_to_channel(uint8_t,int*,int*);
int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t*,void**);
int adc_cali_delete_scheme_curve_fitting(void*);
int adc_cali_raw_to_voltage(void*,int,int*);
'''
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
int adc_oneshot_io_to_channel(uint8_t pin,int* unit,int* channel) {
 if(pin>19)return 1; *unit=0; *channel=pin; return 0;
}
int adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t* c,void** h) {
 if(failCreate)return 1;
 if(c->bitwidth!=12)return 1;
 *h=reinterpret_cast<void*>(static_cast<uintptr_t>(++creates));return 0;
}
int adc_cali_delete_scheme_curve_fitting(void*) {++deletes;return 0;}
int adc_cali_raw_to_voltage(void*,int raw,int* mv) {lastRaw=raw;if(failConvert)return 1;*mv=nextMv;return 0;}
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
            temp = Path(directory); (temp/'esp_adc').mkdir()
            for name in ['adc_cali.h','adc_cali_scheme.h','adc_oneshot.h']:
                (temp/'esp_adc'/name).write_text(sdk if name=='adc_cali.h' else '#include "adc_cali.h"\n', encoding='utf8')
            (temp/'test.cpp').write_text(harness,encoding='utf8')
            binary=temp/('test.exe' if os.name=='nt' else 'test')
            result=subprocess.run([compiler,'-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
                '-I'+str(temp),'-I'+str(ROOT/'include'),str(temp/'test.cpp'),
                str(ROOT/'src/core/AnalogInputCalibration.cpp'),'-o',str(binary)],capture_output=True,text=True,timeout=60)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_opt_in_persists_and_uses_same_sample_everywhere(self):
        persistence=(ROOT/'src/core/PeripheralManagerPersistence.cpp').read_text(encoding='utf8')
        self.assertIn('params["calibratedVoltage"] | false',persistence)
        self.assertIn('params["calibratedVoltage"] = config.params.adc.calibratedVoltage',persistence)
        route=(ROOT/'src/network/handlers/PeripheralRouteHandler.cpp').read_text(encoding='utf8')
        self.assertIn('pm.convertAnalogRawValue(id, rawValue, &quality, &pinVoltage)',route)
        self.assertIn('data["pinVoltageMv"]',route)
        self.assertIn('data["adcPin"] = config.getPrimaryPin()', route)
        self.assertIn('data["pinVoltageSource"]', route)
        self.assertIn('calculateAnalogInputCurrentMa(config, pinVoltage)', route)
        self.assertIn('data["calculatedCurrentMa"] = nullptr', route)
        manager=(ROOT/'src/core/PeripheralManager.cpp').read_text(encoding='utf8')
        self.assertEqual(manager.count('prepareAnalogInput(*config);'),4)
        self.assertIn('if (!adc.calibratedVoltage) return;',manager)


if __name__=='__main__':unittest.main()
