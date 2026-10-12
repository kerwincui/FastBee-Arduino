#include <ArduinoJson.h>
#include "network/SdRecorderConfigJson.h"
#include <iostream>
#include <string>
int main() {
    JsonDocument document;
    FastBee::writeSdRecorderConfigJson(document.to<JsonObject>());
    if (document.overflowed()) return 1;
    std::string text;
    serializeJson(document, text);
    std::cout << text;
}
