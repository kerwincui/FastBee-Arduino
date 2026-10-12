#include "core/SensorDriverMetadata.h"
#include <iostream>
int main() {
    const auto* meta = FastBee::sensorDriverMetadata("ADS1115");
    if (!meta) return 2;
    // All strings below are immutable production metadata, never user input.
    std::cout << "{\"name\":\"" << meta->name << "\",\"addressMin\":" << unsigned(meta->addressMin)
        << ",\"addressMax\":" << unsigned(meta->addressMax) << ",\"minIntervalMs\":" << meta->minIntervalMs << ",\"channels\":[";
    for (unsigned i = 0; i < meta->channelCount; ++i) {
        if (i) std::cout << ',';
        std::cout << "{\"name\":\"" << meta->channels[i].name << "\",\"unit\":\"" << meta->channels[i].unit << "\"";
        if (meta->channels[i].scanMux >= 0) std::cout << ",\"scanMux\":" << unsigned(meta->channels[i].scanMux);
        if (meta->channels[i].singleMuxOnly) std::cout << ",\"singleMuxOnly\":true";
        std::cout << '}';
    }
    std::cout << "],\"params\":[";
    for (unsigned i = 0; i < meta->paramCount; ++i) {
        const auto& item = meta->params[i]; if (i) std::cout << ',';
        std::cout << "{\"key\":\"" << item.name << "\",\"type\":\"" << item.type << "\",\"unit\":\"" << item.unit
            << "\",\"default\":" << item.defaultValue << ",\"min\":" << item.minValue << ",\"max\":" << item.maxValue;
        if (item.choiceCount) {
            std::cout << ",\"choices\":[";
            for (unsigned j = 0; j < item.choiceCount; ++j) { if (j) std::cout << ','; std::cout << item.choices[j]; }
            std::cout << ']';
        }
        std::cout << '}';
    }
    std::cout << "]}";
}
