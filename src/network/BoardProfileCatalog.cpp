#include "network/BoardProfileCatalog.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace FastBee {
namespace {
// All writes are checked, including escaping. Oversized future descriptors fail
// during measureLength(), before HTTP headers are sent, never truncate JSON.
class JsonWriter {
public:
    JsonWriter(char* data, size_t capacity) : data_(data), capacity_(capacity) {}
    void byte(char value) {
        if (length_ < capacity_) data_[length_++] = value;
        else valid_ = false;
    }
    void raw(const char* text) { while (*text) byte(*text++); }
    void text(const char* value) {
        byte('"');
        if (value) for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
            if (*p == '"' || *p == 0x5C) { byte(0x5C); byte(static_cast<char>(*p)); }
            else if (*p < 0x20) {
                static const char HEX[] = "0123456789abcdef";
                byte(0x5C); raw("u00"); byte(HEX[*p >> 4]); byte(HEX[*p & 15]);
            } else byte(static_cast<char>(*p));
        }
        byte('"');
    }
    void number(unsigned long value) {
        char buffer[24];
        const int size = snprintf(buffer, sizeof(buffer), "%lu", value);
        if (size < 0 || static_cast<size_t>(size) >= sizeof(buffer)) { valid_ = false; return; }
        raw(buffer);
    }
    void key(const char* name) { byte(','); text(name); byte(':'); }
    void textField(const char* name, const char* value) { key(name); text(value); }
    void numberField(const char* name, unsigned long value) { key(name); number(value); }
    void flagField(const char* name, bool value) { key(name); raw(value ? "true" : "false"); }
    size_t size() const { return length_; }
    bool valid() const { return valid_; }
private:
    char* data_;
    size_t capacity_;
    size_t length_ = 0;
    bool valid_ = true;
};

const char* boardProfileChip(const BoardProfile& profile) {
    if (profile.id == BoardProfileId::ESP32_C3_GENERIC) return "ESP32-C3";
    if (profile.id == BoardProfileId::ESP32_S3_GENERIC) return "ESP32-S3";
    return profile.id == BoardProfileId::ESP32_C6_DEVKITC_1 ? "ESP32-C6" : "ESP32";
}

void writeProfile(JsonWriter& w, const BoardProfile& p, const BoardProfile* active) {
    w.raw("{\"id\":"); w.text(p.key);
    w.numberField("profileCode", static_cast<uint8_t>(p.id));
    w.textField("key", p.key);
    w.textField("displayName", p.displayName);
    w.textField("version", p.version);
    w.textField("source", p.source);
    w.textField("chip", boardProfileChip(p));
    w.flagField("active", active && strcmp(active->key, p.key) == 0);
    w.flagField("hardwareVerified", false);
    const bool classicModule = p.id == BoardProfileId::ESP32_WROOM_GENERIC || p.id == BoardProfileId::ESP32_WROVER_GENERIC;
    w.textField("scope", classicModule ? "module-policy" :
        (p.id == BoardProfileId::ESP32_C3_GENERIC || p.id == BoardProfileId::ESP32_S3_GENERIC)
        ? "chip-policy" : "board-policy");
    w.flagField("requiresBoardWiringVerification", true);
    w.numberField("pinCount", p.pinCount);
    w.raw(",\"resources\":{\"i2cBusCount\":"); w.number(p.resources.i2cBusCount);
    w.numberField("spiBusCount", p.resources.spiBusCount);
    w.numberField("genericUartCount", p.resources.genericUartCount);
    w.numberField("firstGenericUartPort", p.resources.firstGenericUartPort);
    w.numberField("ledcChannelCount", p.resources.ledcChannelCount);
    w.numberField("ledcTimerCount", p.resources.ledcTimerCount);
    w.numberField("rmtChannelCount", p.resources.rmtChannelCount);
    w.numberField("adcUnitCount", p.resources.adcUnitCount);
    w.numberField("encoderSlotCount", p.resources.encoderSlotCount);
    w.numberField("pcntUnitCount", p.resources.pcntUnitCount);
    w.numberField("pcntChannelsPerUnit", p.resources.pcntChannelsPerUnit);
    w.flagField("sdmmcSupported", p.resources.sdmmcSupported);
    w.raw("},\"defaultPins\":{\"i2cSda\":"); w.number(p.defaults.i2cSda);
    w.numberField("i2cScl", p.defaults.i2cScl);
    w.numberField("spiMiso", p.defaults.spiMiso);
    w.numberField("spiMosi", p.defaults.spiMosi);
    w.numberField("spiSck", p.defaults.spiSck);
    w.numberField("spiCs", p.defaults.spiCs);
    w.numberField("uart0Rx", p.defaults.uart0Rx);
    w.numberField("uart0Tx", p.defaults.uart0Tx);
    w.key("onboardRgb");
    if (p.defaults.onboardRgb == BOARD_PIN_NONE) w.raw("null");
    else w.number(p.defaults.onboardRgb);
    w.raw("},\"pins\":[");
}

void writePin(JsonWriter& w, const BoardPinProfile& p) {
    w.raw("{\"gpio\":"); w.number(p.gpio);
    w.textField("policy", pinPolicyCode(p.policy));
    w.raw(",\"capabilities\":{\"input\":"); w.raw((p.capabilities & PIN_CAP_INPUT) ? "true" : "false");
    w.flagField("output", (p.capabilities & PIN_CAP_OUTPUT) != 0);
    w.flagField("pullUp", (p.capabilities & PIN_CAP_PULL_UP) != 0);
    w.flagField("pullDown", (p.capabilities & PIN_CAP_PULL_DOWN) != 0);
    w.flagField("interrupt", (p.capabilities & PIN_CAP_INTERRUPT) != 0);
    w.flagField("adc", (p.capabilities & PIN_CAP_ADC) != 0);
    w.flagField("dac", (p.capabilities & PIN_CAP_DAC) != 0);
    w.flagField("touch", (p.capabilities & PIN_CAP_TOUCH) != 0);
    w.raw("},\"risks\":[");
    static const struct { uint32_t bit; const char* name; } RISKS[] = {
        {PIN_RISK_FLASH, "flash"}, {PIN_RISK_NOT_EXPOSED, "not_exposed"},
        {PIN_RISK_STRAPPING, "strapping"}, {PIN_RISK_INPUT_ONLY, "input_only"},
        {PIN_RISK_UART0_CONSOLE, "uart0_console"}, {PIN_RISK_USB_SERIAL_JTAG, "usb_serial_jtag"},
        {PIN_RISK_JTAG, "jtag"}, {PIN_RISK_ONBOARD_RGB, "onboard_rgb"},
        {PIN_RISK_LP_I2C, "lp_i2c"}, {PIN_RISK_ADC2_WIFI, "adc2_wifi"}, {PIN_RISK_PSRAM, "psram"}
    };
    bool first = true;
    for (const auto& risk : RISKS) if (p.risks & risk.bit) {
        if (!first) w.byte(',');
        first = false;
        w.text(risk.name);
    }
    w.byte(']');
    if (p.adcUnit != BOARD_PIN_NONE) {
        w.raw(",\"adc\":{\"unit\":"); w.number(p.adcUnit);
        w.numberField("channel", p.adcChannel); w.byte('}');
    }
    w.byte('}');
}
} // namespace

BoardProfileCatalog::BoardProfileCatalog(const BoardProfile* active, const char* compiledChip)
    : profiles_{&getPuzhongPzEsp32Profile(), &getEsp32C6DevKitC1Profile(),
                &getEsp32C3Profile(), &getEsp32S3Profile(true), &getEsp32S3Profile(false),
                &getEsp32ModuleProfile(false), &getEsp32ModuleProfile(true)},
      active_(active), compiledChip_(compiledChip) {}

void BoardProfileCatalog::rewind() {
    profile_ = pin_ = length_ = offset_ = 0;
    stage_ = Stage::ENVELOPE;
}

bool BoardProfileCatalog::nextFragment() {
    if (!valid_ || stage_ == Stage::DONE) return false;
    JsonWriter w(fragment_, sizeof(fragment_));
    switch (stage_) {
        case Stage::ENVELOPE:
            w.raw("{\"success\":true,\"data\":{\"compiledChip\":"); w.text(compiledChip_);
            w.flagField("activeProfileSupported", active_ != nullptr);
            w.textField("activeProfileId", active_ ? active_->key : "");
            w.textField("selectionBasis", "compiled-target-policy");
            w.raw(",\"profiles\":[");
            stage_ = Stage::PROFILE;
            break;
        case Stage::PROFILE:
            if (profile_) w.byte(',');
            writeProfile(w, *profiles_[profile_], active_);
            pin_ = 0;
            stage_ = profiles_[profile_]->pinCount ? Stage::PIN : Stage::PROFILE_END;
            break;
        case Stage::PIN:
            if (pin_) w.byte(',');
            writePin(w, profiles_[profile_]->pins[pin_++]);
            if (pin_ == profiles_[profile_]->pinCount) stage_ = Stage::PROFILE_END;
            break;
        case Stage::PROFILE_END:
            w.raw("]}");
            stage_ = ++profile_ == sizeof(profiles_) / sizeof(profiles_[0]) ? Stage::END : Stage::PROFILE;
            break;
        case Stage::END:
            w.raw("]}}"); stage_ = Stage::DONE;
            break;
        case Stage::DONE: return false;
    }
    valid_ = w.valid();
    length_ = valid_ ? w.size() : 0;
    offset_ = 0;
    return valid_;
}

size_t BoardProfileCatalog::measureLength() {
    rewind();
    size_t total = 0;
    while (nextFragment()) total += length_;
    rewind();
    return valid_ ? total : 0;
}

size_t BoardProfileCatalog::read(uint8_t* destination, size_t capacity) {
    if (!destination || !valid_) return 0;
    size_t written = 0;
    while (written < capacity) {
        if (offset_ == length_ && !nextFragment()) break;
        const size_t count = std::min(capacity - written, length_ - offset_);
        memcpy(destination + written, fragment_ + offset_, count);
        offset_ += count;
        written += count;
    }
    return written;
}
} // namespace FastBee
