#pragma once
#include "../../../mocks/Arduino.h"
inline unsigned long rfidTestClock = 0;
inline unsigned long rfidTestMillis() { return rfidTestClock; }
#define millis rfidTestMillis
