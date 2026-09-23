#pragma once
#include <Arduino.h>
#include <map>
#include <vector>
class TwoWire {
public:
    enum class Device { INA226, BMP280, MPU6050, STREAM };
    Device device = Device::INA226;
    std::map<uint8_t, uint16_t> registers;
    std::map<uint8_t, uint8_t> byteRegisters;
    std::vector<uint8_t> stream;
    std::vector<uint8_t> output;
    std::vector<uint8_t> input;
    size_t offset = 0;
    uint8_t selected = 0;
    bool ack = true;
    bool shortRead = false;
    bool conversionReady = true;
    bool overflow = false;
    bool loseCalibration = false;
    bool resetStuck = false;
    unsigned conversions = 0;
    unsigned ahtCalibrations = 0;
    void reset(Device mode = Device::INA226) {
        device = mode;
        registers = {{0xfe, 0x5449}, {0xff, 0x2261}, {0x02, 9600}, {0x04, 1000}, {0x01, 100}};
        byteRegisters.clear(); stream.clear();
        output.clear(); input.clear(); offset = 0;
        ack = true; shortRead = overflow = loseCalibration = false;
        conversionReady = true; conversions = 0;
        resetStuck = false;
        ahtCalibrations = 0;
    }
    void beginTransmission(uint8_t) { output.clear(); }
    size_t write(uint8_t byte) { output.push_back(byte); return 1; }
    size_t write(const uint8_t* bytes, size_t length) {
        output.insert(output.end(), bytes, bytes + length); return length;
    }
    uint8_t endTransmission(bool = true) {
        if (!ack) return 2;
        if (output.size()) selected = output[0];
        if (device == Device::STREAM) {
            if (output == std::vector<uint8_t>({0xbe, 0x08, 0x00})) {
                ++ahtCalibrations;
                if (!stream.empty()) stream[0] |= 0x08;
            }
            return 0;
        }
        if (device != Device::INA226) {
            for (size_t i = 1; i < output.size(); ++i) byteRegisters[selected + i - 1] = output[i];
            if (device == Device::MPU6050 && selected == 0x6b && output.size() == 2 &&
                output[1] == 0x80 && !resetStuck) byteRegisters[0x6b] = 0x40;
            if (device == Device::BMP280 && selected == 0xf4 && output.size() == 2 &&
                (output[1] & 3) == 1) { ++conversions; byteRegisters[0xf3] = conversionReady ? 0 : 8; }
            return 0;
        }
        if (output.size() == 3) {
            registers[selected] = static_cast<uint16_t>((output[1] << 8) | output[2]);
            if (selected == 5 && loseCalibration) registers[5] = 0;
            if (selected == 0 && (registers[0] & 7) == 3) {
                ++conversions;
                registers[6] = (conversionReady ? 8 : 0) | (overflow ? 4 : 0);
            }
        }
        return 0;
    }
    size_t requestFrom(uint8_t, uint8_t length) {
        if (!ack) return 0;
        offset = 0;
        if (device != Device::INA226) {
            input.clear();
            if (device == Device::STREAM) input = stream;
            else for (uint8_t i = 0; i < length; ++i) input.push_back(byteRegisters[selected + i]);
            if (input.size() > length) input.resize(length);
            if (shortRead && !input.empty()) input.pop_back();
            return input.size();
        }
        const uint16_t value = registers[selected];
        input = {static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        offset = 0;
        if (selected == 6) registers[6] &= ~8;
        if (shortRead) { input.resize(1); return 1; }
        return std::min<size_t>(length, input.size());
    }
    int available() const { return static_cast<int>(input.size() - offset); }
    int read() { return offset < input.size() ? input[offset++] : -1; }
};
inline TwoWire Wire;
