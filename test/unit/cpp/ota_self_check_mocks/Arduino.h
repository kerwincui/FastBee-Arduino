#pragma once
#include <stdint.h>
unsigned long millis();
void delay(unsigned long milliseconds);
struct TestSerial {
    template<class... Args> void printf(const char*, Args...) {}
    void println(const char*) {}
    void flush() {}
};
struct TestEsp { void restart(); };
extern TestSerial Serial;
extern TestEsp ESP;
