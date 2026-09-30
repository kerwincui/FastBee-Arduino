"""Execute SPI production brightness/sleep methods with injected LEDC failures."""
import os
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def method(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end].replace(' override', '')


class ScreenBacklightTests(unittest.TestCase):
    def test_shared_backlight_truth_table_including_ch32_pwm(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler and pathlib.Path('D:/msys64/ucrt64/bin/g++.exe').exists():
            compiler = 'D:/msys64/ucrt64/bin/g++.exe'
        if not compiler:
            self.skipTest('C++17 compiler required')
        cpp = r"""
#include <cassert>
#include <cstdint>
#include "screen/ScreenBacklight.h"
using FastBee::Screen::backlightDuty;
using FastBee::Screen::normalizeBrightnessPercent;
struct PwmCase { uint8_t percent; bool sleeping; bool activeHigh; uint8_t duty; };
int main() {
    assert(normalizeBrightnessPercent(0) == 0);
    assert(normalizeBrightnessPercent(1) == 1);
    assert(normalizeBrightnessPercent(10) == 10);
    assert(normalizeBrightnessPercent(50) == 50);
    assert(normalizeBrightnessPercent(100) == 100);
    assert(normalizeBrightnessPercent(255) == 100);
    const PwmCase table[] = {
        {0, false, true, 26}, {1, false, true, 28}, {10, false, true, 48},
        {50, false, true, 140}, {100, false, true, 255},
        {0, false, false, 229}, {1, false, false, 227}, {10, false, false, 207},
        {50, false, false, 115}, {100, false, false, 0},
        {50, true, true, 0}, {50, true, false, 255},
    };
    for (const auto& entry : table) {
        assert(backlightDuty(entry.percent, entry.sleeping, entry.activeHigh) == entry.duty);
    }
    // Bind the public board sample to production duty calculations. The user confirmed
    // that high electrical duty dims this board; other boards keep their own polarity.
    const bool panelActiveHigh = PANEL_ACTIVE_HIGH;
    assert(!panelActiveHigh);
    assert(backlightDuty(0, false, panelActiveHigh) == 229);
    assert(backlightDuty(100, false, panelActiveHigh) == 0);
    assert(backlightDuty(70, true, panelActiveHigh) == 255);
    for (int percent = 1; percent <= 100; ++percent) {
        assert(backlightDuty(percent, false, panelActiveHigh) <
               backlightDuty(percent - 1, false, panelActiveHigh));
        assert(backlightDuty(percent, false, panelActiveHigh) != 255);
    }
}
"""
        sample = json.loads((ROOT / 'docs/waveshare-esp32s3-touch-lcd4/peripherals.json').read_text(encoding='utf-8'))
        panel = next(item for item in sample['peripherals'] if item['id'] == 'panel_display')
        cpp = cpp.replace('PANEL_ACTIVE_HIGH', json.dumps(panel['params']['backlightActiveHigh']))
        with tempfile.TemporaryDirectory(prefix='fastbee-backlight-table-') as temporary:
            folder = pathlib.Path(temporary)
            (folder / 'table.cpp').write_text(cpp, encoding='utf-8')
            executable = folder / ('table.exe' if os.name == 'nt' else 'table')
            env = dict(os.environ)
            env['PATH'] = str(pathlib.Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I' + str(ROOT / 'include'), str(folder / 'table.cpp'),
                                     '-o', str(executable)], capture_output=True, text=True,
                                    timeout=30, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_spi_polarities_minimum_sleep_restore_and_write_failure(self):
        compiler = shutil.which('g++') or shutil.which('clang++')
        if not compiler and pathlib.Path('D:/msys64/ucrt64/bin/g++.exe').exists():
            compiler = 'D:/msys64/ucrt64/bin/g++.exe'
        if not compiler:
            self.skipTest('C++17 compiler required')
        source = (ROOT / 'src/screen/SpiTftDisplayBackend.cpp').read_text(encoding='utf-8')
        methods = '\n'.join(method(source, signature) for signature in (
            'bool setBrightness(uint8_t percent)', 'bool setSleep(bool sleeping)',
            'bool applyBrightness(uint8_t percent, bool sleeping)'))
        cpp = r'''
#include <cassert>
#include <vector>
#include "screen/ScreenBacklight.h"
using FastBee::Screen::backlightDuty;
using FastBee::Screen::normalizeBrightnessPercent;
constexpr int CMD_SLPIN=0x10,CMD_SLPOUT=0x11;
int duty=-1;bool failed=false;
bool ledcWrite(uint8_t,int value){if(failed)return false;duty=value;return true;}
void delay(int){}
struct Spi {
 struct {int backlight=1;bool backlightActiveHigh=true;} _config;
 bool _ready=true,_sleeping=false,_backlightAttached=true;
 uint8_t _brightness=70;
 std::vector<int> commands;
 bool writeCommand(int cmd,void*,int){commands.push_back(cmd);return true;}
 METHODS
};
int main(){
 for(bool active:{false,true}){
  Spi s;s._config.backlightActiveHigh=active;
  auto expect=[&](int value){return active?value:255-value;};
  assert(s.setBrightness(0)&&duty==expect(26));
  int prior=duty;
  for(int value=1;value<=100;++value){assert(s.setBrightness(value));assert(active?duty>=prior:duty<=prior);prior=duty;}
  assert(duty==expect(255));
  assert(s.setBrightness(255)&&duty==expect(255));
  assert(s.setBrightness(30)&&duty==expect(94));
  failed=true;assert(!s.setBrightness(99));assert(s._brightness==30);
  assert(!s.setSleep(true)&&!s._sleeping&&s.commands.back()==CMD_SLPOUT);
  failed=false;assert(s.setSleep(true)&&duty==expect(0)&&s.commands.back()==CMD_SLPIN);
  assert(s.setBrightness(70)&&duty==expect(0));
  assert(s.setSleep(false)&&duty==expect(186)&&s.commands.back()==CMD_SLPOUT);
  assert(s.setBrightness(0)&&s.setSleep(true)&&s.setSleep(false)&&duty==expect(26));
 }
 Spi noPwm;noPwm._config.backlight=-1;
 assert(noPwm.setBrightness(100));assert(!noPwm.setBrightness(0));
 assert(noPwm.setSleep(true)&&noPwm.setSleep(false));
}
'''.replace('METHODS', methods)
        with tempfile.TemporaryDirectory(prefix='fastbee-backlight-') as temporary:
            folder = pathlib.Path(temporary)
            (folder / 'backlight.cpp').write_text(cpp, encoding='utf-8')
            executable = folder / ('backlight.exe' if os.name == 'nt' else 'backlight')
            env = dict(os.environ)
            env['PATH'] = str(pathlib.Path(compiler).parent) + os.pathsep + env.get('PATH', '')
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     '-I' + str(ROOT / 'include'), str(folder / 'backlight.cpp'),
                                     '-o', str(executable)], capture_output=True, text=True,
                                    timeout=30, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True,
                                    timeout=10, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
