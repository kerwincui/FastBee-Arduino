#include "core/ScriptEngine.h"
#include "core/ScriptRandomExpansion.h"
#include <cassert>
#include <utility>

struct Logger {
    void warning(const char*) {}
} LOGGER;
// @production@

static void reject(const std::string& text) {
    assert(ScriptEngine::parse(String(text)).empty());
}
int main() {
    const auto waveform = ScriptEngine::parse("PERIPH led TOGGLE\nPERIPH led BLINK 0\nPERIPH led BLINK 65535\nPERIPH buzzer PWM 127\nPERIPH out DAC 255");
    assert(waveform.size() == 5 && waveform[0].subAction == "TOGGLE");
    assert(waveform[1].intParam == 0 && waveform[2].intParam == 65535);
    assert(waveform[3].intParam == 127 && waveform[4].intParam == 255);
    for (const char* text : {"PERIPH led TOGGLE 1", "PERIPH led TOGGLE extra",
                            "PERIPH led BLINK 65536", "PERIPH out DAC 256", "PERIPH out DAC -1"}) reject(text);
    auto commands=ScriptEngine::parse("gpio\t5\thigh\r\n#comment\n\nDELAY -100\nPWM 4 255\nDAC 25 300\nDAC 25 -10\nMQTT +0 hello   world\nPERIPH lcd SHOW -1\nPERIPH led BLINK 65535\nPERIPH seg TEXT A   B\nPERIPH led BRIGHTNESS -1");
    assert(commands.size()==10);
    assert(commands[0].pin==5 && commands[0].intParam==1);
    assert(commands[1].intParam==0);
    assert(commands[2].intParam==255 && commands[3].intParam==255 && commands[4].intParam==0);
    assert(commands[5].intParam==0 && commands[5].strParam=="hello world");
    assert(commands[6].extraParam=="-1" && commands[7].intParam==65535);
    assert(commands[8].extraParam=="A B" && commands[9].intParam==-1);
    for(const char* text:{"GPIO abc HIGH", "GPIO -1 HIGH", "GPIO 256 HIGH", "GPIO 65536 LOW",
            "GPIO 5x HIGH", "GPIO 1.5 HIGH", "GPIO 0x10 HIGH", "GPIO 5 HIGH extra",
            "PWM 5 -1", "PWM 5 256", "PWM 5 junk", "PWM 5 100 extra",
            "DAC 25 abc", "DAC 25 2147483648", "DELAY junk", "DELAY 1x", "DELAY 1 2",
            "MQTT junk message", "MQTT -1 message", "MQTT 2147483648 message", "MQTT 1.5 data",
            "PERIPH led HIGH extra", "PERIPH led BLINK -1", "PERIPH led BREATHE 65536",
            "PERIPH led PWM -1", "PERIPH led BRIGHTNESS bad", "PERIPH lcd SHOW 128",
            "PERIPH lcd SHOW -2", "PERIPH lcd SHOW bad", "PERIPH lcd SHOW 0 extra"}) {
        reject(text);
        reject(std::string("GPIO 5 HIGH\n")+text); // No valid prefix escapes a failed later line.
    }
    reject(std::string("GPIO 5 HIGH\0\nGPIO 5 LOW",23));
    reject(std::string(1025,'x'));
    std::string fifty;
    for(int i=0;i<50;++i) fifty+="LOG x\n";
    assert(ScriptEngine::parse(String(fifty)).size()==50);
    reject(fifty+"LOG y");
    auto valid=ScriptEngine::parse("DELAY 10000\nDELAY 10000\nDELAY 10000");
    String error;
    assert(ScriptEngine::validate(valid,error));
    assert(!ScriptEngine::validate(ScriptEngine::parse("DELAY 10001"),error));
    assert(!ScriptEngine::validate(ScriptEngine::parse("DELAY 10000\nDELAY 10000\nDELAY 10000\nDELAY 1"),error));
    assert(!ScriptEngine::validate(ScriptEngine::parse("GPIO 6 HIGH"),error));

    const String script("GPIO 5 HIGH\nLOG hello world\nMQTT 1 payload here\nPERIPH led HIGH\nPERIPH lcd SHOW 1\nPERIPH lcd TEXT hello world");
    String::allocations=0;
    assert(ScriptEngine::parse(script).size()==6);
    const int points=String::allocations;
    assert(points>20);
    for(int point=0;point<points;++point) {
        String::allocations=0;String::failAt=point;
        assert(ScriptEngine::parse(script).empty());
    }
    String::failAt=-1;
    assert(ScriptEngine::parse(script).size()==6);
}
