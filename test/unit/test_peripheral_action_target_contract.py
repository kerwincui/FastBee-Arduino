"""Compile actual preflight/save target predicates and compare their behavior."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_sensor_publication import definition

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <cmath>
using std::isnan;
using std::isinf;
#include "core/PeripheralConfig.h"
#include "core/PeripheralExecution.h"
#include <cassert>
#include <cstdio>
#if __has_include("core/PeripheralActionContract.h")
#include "core/PeripheralActionContract.h"
#endif
#include "target_predicates.inc"
int main() {
    PeripheralConfig config;
    config.pinCount = 1;
    config.params.gpio.initialState = GPIOState::STATE_HIGH;
    config.params.gpio.invertLogic = false;
    for (unsigned action = 0; action <= 40; ++action) {
        if (action == 20) continue; // Reserved: rejected before target admission.
        if (actionNeedsPeripheral(action) != actionRequiresPeripheralTarget(action)) {
            std::fprintf(stderr, "required-target mismatch action=%u\n", action); return 1;
        }
        for (unsigned type = 0; type < 100; ++type) for (unsigned mode = 0; mode < 4; ++mode) {
            config.type = static_cast<PeripheralType>(type);
            config.params.gpio.ioSource = mode & 1;
            config.params.gpio.passive = (mode & 2) != 0;
            if (actionTargetMatches(action, config) != actionMatchesPeripheralTarget(action, config)) {
                std::fprintf(stderr, "target mismatch action=%u type=%u expander=%u passive=%u\n",
                             action, type, mode & 1, (mode & 2) != 0); return 1;
            }
        }
    }
    config.type = PeripheralType::PWM_SERVO;
    assert(actionTargetMatches(4, config));
    config.type = PeripheralType::ENCODER;
    assert(actionTargetMatches(19, config));
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.params.gpio.ioSource = 0; config.params.gpio.passive = false;
    assert(!actionTargetMatches(3, config) && !actionTargetMatches(4, config));
    config.params.gpio.passive = true;
    assert(actionTargetMatches(4, config));
    config.params.gpio.ioSource = 1;
    assert(!actionTargetMatches(4, config));
    config.type = PeripheralType::GPIO_ANALOG_OUTPUT;
    config.params.gpio.ioSource = 0;
    assert(!actionTargetMatches(2, config) && !actionTargetMatches(5, config));
    config.type = PeripheralType::DS1302_RTC;
    assert(actionTargetMatches(35, config));
    assert(actionTargetMatches(36, config));
    assert(actionTargetMatches(37, config));
    config.type = PeripheralType::UART;
    assert(!actionTargetMatches(35, config) && actionTargetMatches(29, config));
    assert(!actionTargetMatches(36, config));
    assert(!actionTargetMatches(37, config));
    config.type = PeripheralType::SDIO;
    assert(actionTargetMatches(38, config) && !actionTargetMatches(39, config));
    config.type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    config.params.gpio.ioSource = 0; config.params.gpio.passive = false;
    assert(actionTargetMatches(39, config));
    config.pinCount = 0; assert(!actionTargetMatches(39, config));
    config.pinCount = 1; config.params.gpio.initialState = GPIOState::STATE_LOW;
    assert(!actionTargetMatches(39, config));
    config.params.gpio.initialState = GPIOState::STATE_HIGH; config.params.gpio.invertLogic = true;
    assert(!actionTargetMatches(39, config));
}
'''

class PeripheralActionTargetContractTests(unittest.TestCase):
    def test_migration_preview_matches_real_cpp_target_predicate(self):
        # Prepared only: compare actual C++ against the real imported JS helper,
        # including native/expander and active/passive combinations. No device.
        compiler = shutil.which('g++') or shutil.which('clang++')
        node = shutil.which('node')
        if not compiler or not node:
            raise RuntimeError('C++17 compiler and Node required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            installed = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not installed:
                raise RuntimeError('Installed ArduinoJson headers required')
            json_root = str(installed[0].parent)
        program = r'''
#include <cmath>
using std::isnan; using std::isinf;
#include <cstdio>
#include "core/PeripheralActionContract.h"
int main() {
    using namespace FastBee::PeripheralActionContract;
    PeripheralConfig config;
    for (unsigned action=0;action<=39;++action) {
        if (!requiresPeripheralTarget(action)) continue;
        for (unsigned type=0;type<100;++type) for (unsigned mode=0;mode<4;++mode)
        for (unsigned pins=action==39?0:1;pins<(action==39?3:2);++pins)
        for (unsigned invert=0;invert<(action==39?2:1);++invert)
        for (unsigned initial=action==39?0:1;initial<2;++initial) {
            config.type=static_cast<PeripheralType>(type);
            config.params.gpio.ioSource=mode&1;
            config.params.gpio.passive=(mode&2)!=0;
            config.pinCount=pins;
            config.params.gpio.invertLogic=invert!=0;
            config.params.gpio.initialState=initial?GPIOState::STATE_HIGH:GPIOState::STATE_LOW;
            std::printf("%u %u %u %u %u %u %u\n",action,type,mode,pins,invert,initial,matchesPeripheralTarget(action,config)?1:0);
        }
    }
}
'''
        compare = r'''
const fs=require('fs'),vm=require('vm'),assert=require('assert/strict');
const app={registerModule(n,m){Object.assign(this,m);this.setupDeviceConfigEvents=()=>{};}};
vm.runInNewContext(fs.readFileSync('web-src/modules/runtime/device-config-transfer.js','utf8'),{AppState:app,window:{},console});
for(const line of fs.readFileSync(0,'utf8').trim().split(/\r?\n/)) {
    const [a,t,m,pins,invert,initial,want]=line.split(' ').map(Number);
    const got=app._matchesConfigTransferActionTarget(a,{type:t,pins:Array(pins).fill(2),
        params:{ioSource:m&1,passive:!!(m&2),invertLogic:!!invert,initialState:initial}});
    assert.equal(got,!!want,`action=${a} type=${t} mode=${m} pins=${pins} invert=${invert} initial=${initial}`);
}
'''
        with tempfile.TemporaryDirectory(prefix='fastbee-migration-targets-') as folder:
            temp = Path(folder)
            source = temp / 'targets.cpp'
            source.write_text(program, encoding='utf-8')
            binary = temp / ('targets.exe' if os.name == 'nt' else 'targets')
            built = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-I'+json_root, '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'),
                str(source), '-o', str(binary)], capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout+built.stderr)
            rows = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(rows.returncode, 0, rows.stdout+rows.stderr)
            self.assertTrue(rows.stdout.strip())
            checked = subprocess.run([node, '-e', compare], input=rows.stdout, cwd=ROOT,
                capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=30)
            self.assertEqual(checked.returncode, 0, checked.stdout+checked.stderr)

    def test_preflight_and_save_use_identical_typed_targets(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler: self.skipTest('C++17 compiler required')
        json_root = os.environ.get('FASTBEE_TEST_ARDUINOJSON_INCLUDE')
        if not json_root:
            installed = list((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h'))
            if not installed: self.skipTest('Installed ArduinoJson headers required')
            json_root = str(installed[0].parent)
        manager = (ROOT / 'src/core/PeriphExecManager.cpp').read_text(encoding='utf-8')
        route = (ROOT / 'src/network/handlers/PeriphExecRouteHandler.cpp').read_text(encoding='utf-8')
        functions = []
        for source, signatures in ((manager, ('static bool isRuleSensorTarget(',
                'static bool actionRequiresPeripheralTarget(', 'static bool actionMatchesPeripheralTarget(')),
                (route, ('bool isOutputPeripheral(', 'bool isSensorPeripheral(',
                         'bool actionNeedsPeripheral(', 'bool actionTargetMatches('))):
            for signature in signatures:
                if signature in source: functions.append(definition(source, signature))
        with tempfile.TemporaryDirectory(prefix='fastbee-action-targets-') as folder:
            temp = Path(folder)
            (temp / 'target_predicates.inc').write_text('\n'.join(functions), encoding='utf-8')
            (temp / 'harness.cpp').write_text(HARNESS, encoding='utf-8')
            binary = temp / ('targets.exe' if os.name == 'nt' else 'targets')
            command = [compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                       '-I'+json_root,
                       '-I'+str(ROOT/'test/mocks'), '-I'+str(ROOT/'include'),
                       str(temp/'harness.cpp'), '-o', str(binary)]
            result = subprocess.run(command, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)

if __name__ == '__main__': unittest.main()
