"""Real U8g2 GPIO initialization must not steal the managed Wire pins."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test.unit.test_panel_runtime_regressions import block

ROOT = Path(__file__).resolve().parents[2]


class LcdManagedI2cGpioTests(unittest.TestCase):
    def test_begin_and_maintenance_preserve_bus_reset_and_delays(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        self.assertIsNotNone(compiler, "C++17 compiler required")
        candidates = list((ROOT / ".pio/libdeps").glob("*/U8g2/src/U8x8lib.cpp"))
        self.assertTrue(candidates, "Installed production U8g2 dependency required")
        library = candidates[0].parent
        gpio_source = (library / "U8x8lib.cpp").read_text(encoding="utf-8")
        display_source = (library / "clib/u8x8_display.c").read_text(encoding="utf-8")
        manager = (ROOT / "src/peripherals/LCDManager.cpp").read_text(encoding="utf-8")
        # Execute the complete production binding block, not a reimplementation.
        binding_start = manager.rfind("if (_interface == DisplayInterface::I2C_MODE)",
                                     0, manager.index("->byte_cb = managedU8g2I2cByte"))
        binding = block(manager[binding_start:], "if (")
        callback_signature = 'extern "C" uint8_t u8x8_gpio_and_delay_arduino(u8x8_t* u8x8, uint8_t msg, uint8_t arg_int, U8X8_UNUSED void* arg_ptr)'
        callback = callback_signature + block(gpio_source, 'extern "C" uint8_t u8x8_gpio_and_delay_arduino(')
        helper = "void u8x8_d_helper_display_init(u8x8_t* u8x8)" + block(
            display_source, "void u8x8_d_helper_display_init(")
        source = r'''
#include <cassert>
#include <cstring>
#define ARDUINO 1
#include "u8x8.h"
#define OUTPUT 1
#define INPUT_PULLUP 2
bool wireInitialized = true;
int resetPinModes = 0, resetWrites = 0, delayMs = 0, byteInits = 0;
void pinMode(uint8_t pin, int) {
    // Arduino 3 periman releases the I2C controller if GPIO claims SDA/SCL.
    if(pin == 5 || pin == 6) wireInitialized = false;
    if(pin == 9) ++resetPinModes;
}
void digitalWrite(uint8_t pin, int) { if(pin == 9) ++resetWrites; }
int digitalRead(uint8_t) { return 1; }
void delay(int ms) { delayMs += ms; }
void delayMicroseconds(int) {}
void yield() {}
CALLBACK
extern "C" void u8x8_gpio_call(u8x8_t* u, uint8_t msg, uint8_t arg) {
    u->gpio_and_delay_cb(u,msg,arg,nullptr);
}
HELPER
uint8_t managedU8g2I2cByte(u8x8_t*, uint8_t msg, uint8_t, void*) {
    if(msg == U8X8_MSG_BYTE_INIT) ++byteInits;
    // A managed lease still exists even when Arduino's GPIO handler stole it.
    return 1;
}
uint8_t cad(u8x8_t* u, uint8_t msg, uint8_t, void*) {
    assert(msg == U8X8_MSG_CAD_INIT);
    return u->byte_cb(u,U8X8_MSG_BYTE_INIT,0,nullptr);
}
struct Display {
    u8x8_t u{};
    u8x8_t* getU8x8() { return &u; }
    void setBusClock(uint32_t clock) { assert(clock == 100000); }
};
void bindManagedPins(Display* _display) BINDING
int main() {
    Display d;
    std::memset(d.u.pins,U8X8_PIN_NONE,sizeof(d.u.pins));
    u8x8_SetPin(&d.u,U8X8_PIN_I2C_CLOCK,6);
    u8x8_SetPin(&d.u,U8X8_PIN_I2C_DATA,5);
    u8x8_SetPin(&d.u,U8X8_PIN_RESET,9);
    d.u.gpio_and_delay_cb=u8x8_gpio_and_delay_arduino;
    d.u.byte_cb=managedU8g2I2cByte;
    d.u.cad_cb=cad;
    u8x8_display_info_t info{};
    info.reset_pulse_width_ms=2; info.post_reset_wait_ms=3;
    d.u.display_info=&info;
    // Regression control: the old binding does destroy an initialized Wire.
    u8x8_d_helper_display_init(&d.u);
    assert(!wireInitialized && byteInits == 1);
    bindManagedPins(&d);
    wireInitialized=true;resetPinModes=resetWrites=delayMs=byteInits=0;
    // begin and every later maintenance initDisplay share this real helper.
    for(int i=0;i<4;++i) {
        u8x8_d_helper_display_init(&d.u);
        assert(wireInitialized);
    }
    assert(byteInits==4 && resetPinModes==4 && resetWrites==12 && delayMs==28);
    assert(d.u.pins[U8X8_PIN_RESET]==9);
    assert(d.u.gpio_and_delay_cb==u8x8_gpio_and_delay_arduino);
}
'''.replace("CALLBACK", callback).replace("HELPER", helper).replace("BINDING", binding)
        with tempfile.TemporaryDirectory(prefix="fastbee-lcd-gpio-") as directory:
            base = Path(directory)
            cpp, executable = base / "case.cpp", base / ("case.exe" if os.name == "nt" else "case")
            cpp.write_text(source, encoding="utf-8")
            result = subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                                     f"-I{library / 'clib'}", str(cpp), "-o", str(executable)],
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
