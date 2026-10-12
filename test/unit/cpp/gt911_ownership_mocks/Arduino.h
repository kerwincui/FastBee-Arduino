#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
class String : public std::string {
public: using std::string::string; using std::string::operator=;
    bool isEmpty() const {return empty();}
};
enum { INPUT=0, OUTPUT=1, LOW=0, HIGH=1 };
inline uint32_t mockNow=0;
inline unsigned mockPinModes=0, mockPinWrites=0;
inline uint32_t millis() {return mockNow;}
inline void delay(uint32_t ms) {mockNow+=ms;}
inline void pinMode(uint8_t,int) {++mockPinModes;}
inline void digitalWrite(uint8_t,int) {++mockPinWrites;}
