#pragma once
#define ESP MigrationBaseESP
#include "../../../mocks/Arduino.h"
#undef ESP
struct MigrationESP {
    String firmware="abcdef0123456789abcdef0123456789";
    String getSketchMD5()const{return firmware;}
};
inline MigrationESP ESP;
