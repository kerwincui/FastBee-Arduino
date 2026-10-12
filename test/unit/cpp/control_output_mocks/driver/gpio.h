#pragma once
#include <cassert>
#include <vector>
#include "core/AsyncExecTypes.h"
using gpio_num_t=int;
constexpr int ESP_OK=0,GPIO_MODE_INPUT_OUTPUT=3;
inline int levels[49]={},setCalls=0,directionCalls=0,failSetAt=0,forcedRead=-1;
inline bool failDirection=false;
inline std::vector<int> commanded;
inline unsigned waitedUs=0;
inline unsigned readCalls=0;
inline int gpio_set_level(gpio_num_t pin,int level) {
    assert(controlDepth>0); ++setCalls;commanded.push_back(level);
    if(setCalls==failSetAt)return -1;
    levels[pin]=level;return ESP_OK;
}
inline int gpio_set_direction(gpio_num_t,int mode) {
    assert(controlDepth>0 && mode==GPIO_MODE_INPUT_OUTPUT);++directionCalls;
    return failDirection?-1:ESP_OK;
}
inline int gpio_get_level(gpio_num_t pin) { assert(controlDepth>0);++readCalls;return forcedRead<0?levels[pin]:forcedRead; }
inline void delayMicroseconds(unsigned value) { assert(controlDepth>0 && value==2);waitedUs+=value; }
