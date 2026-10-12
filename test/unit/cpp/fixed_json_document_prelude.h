#pragma once
#include <Arduino.h>
// Use the same real ArduinoJson allocator definitions in every fixture TU.
// Explicit allocator faults below do not use the general mock heap macros.
#undef malloc
#undef free
