#pragma once
#include <cassert>
#include <mutex>
using UBaseType_t = unsigned;
using BaseType_t = int;
using portMUX_TYPE = std::recursive_mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
inline thread_local unsigned criticalDepth = 0;
#define portENTER_CRITICAL(m) do { (m)->lock(); ++criticalDepth; } while (0)
#define portEXIT_CRITICAL(m) do { assert(criticalDepth); --criticalDepth; (m)->unlock(); } while (0)
