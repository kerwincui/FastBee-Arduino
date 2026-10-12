#include "network/handlers/PeripheralRouteHandler.h"
#include "network/handlers/HandlerUtils.h"
#include "network/JsonFragmentsResponse.h"
#include "network/FixedJsonField.h"
#include "network/ThermostatConfigJson.h"
#include "network/SdRecorderConfigJson.h"
#include "network/WebHandlerContext.h"
#include "core/MemoryBudget.h"
#include "core/FeatureFlags.h"
#include "core/ChipConfig.h"
#include "core/PeripheralManager.h"
#include "core/ResourceProfile.h"
#include "core/DriverRegistry.h"
#include "core/H1ParameterJson.h"
#include "core/RuntimeFeatureManager.h"
#include "utils/PsramJsonDocument.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <initializer_list>
#include <memory>

namespace {
void addSensorDriverDescriptors(JsonObject typeObj) {
    const FastBee::DriverRegistry& registry = FastBee::DriverRegistry::getInstance();
    JsonArray names = typeObj["drivers"].to<JsonArray>();
    JsonArray descriptors = typeObj["driverDescriptors"].to<JsonArray>();
    FastBee::setFixedJsonField(typeObj, "featureActive", FastBee::RuntimeFeatureManager::instance().shouldEnable("i2cSensors"));
    FastBee::setFixedJsonField(typeObj, "registrationFailures", registry.getRegistrationFailureCount());
    FastBee::setFixedJsonField(typeObj, "registeredDriverCount", registry.getDriverCount());
    FastBee::setFixedJsonField(typeObj, "driverCapacity", registry.getCapacity());
    FastBee::setFixedJsonField(typeObj, "availableDriverSlots", registry.getAvailableSlots());
    for (size_t i = 0; i < registry.getDriverCount(); ++i) {
        const FastBee::SensorDriverEntry* entry = registry.getDriverEntry(i);
        if (!entry || !entry->metadata) continue;
        const FastBee::SensorDriverMetadata& metadata = *entry->metadata;
        names.add(metadata.name);
        JsonObject descriptor = descriptors.add<JsonObject>();
        FastBee::setFixedJsonField(descriptor, "name", metadata.name);
        FastBee::setFixedJsonField(descriptor, "busType", "i2c");
        FastBee::setFixedJsonField(descriptor, "compiled", true);
        FastBee::setFixedJsonField(descriptor, "defaultAddress", metadata.defaultAddress);
        FastBee::setFixedJsonField(descriptor, "addressMin", metadata.addressMin);
        FastBee::setFixedJsonField(descriptor, "addressMax", metadata.addressMax);
        FastBee::setFixedJsonField(descriptor, "minIntervalMs", metadata.minIntervalMs);
        FastBee::setFixedJsonField(descriptor, "featureKey", metadata.featureKey);
        FastBee::setFixedJsonField(descriptor, "featureActive", !metadata.featureKey ||
            FastBee::RuntimeFeatureManager::instance().shouldEnable(metadata.featureKey));
        JsonArray addresses = descriptor["addresses"].to<JsonArray>();
        for (uint8_t n = 0; n < metadata.addressCount; ++n) {
            addresses.add(metadata.addresses[n]);
        }
        JsonArray channels = descriptor["channels"].to<JsonArray>();
        for (uint8_t n = 0; n < metadata.channelCount; ++n) {
            JsonObject channel = channels.add<JsonObject>();
            FastBee::setFixedJsonField(channel, "name", metadata.channels[n].name);
            FastBee::setFixedJsonField(channel, "unit", metadata.channels[n].unit);
            FastBee::setFixedJsonField(channel, "mode", metadata.channels[n].mode);
            if (metadata.channels[n].scanMux >= 0)
                FastBee::setFixedJsonField(channel, "scanMux", metadata.channels[n].scanMux);
            if (metadata.channels[n].singleMuxOnly)
                FastBee::setFixedJsonField(channel, "singleMuxOnly", true);
        }
        JsonArray params = descriptor["params"].to<JsonArray>();
        for (uint8_t n = 0; n < metadata.paramCount; ++n) {
            const FastBee::SensorParameterMetadata& parameter = metadata.params[n];
            JsonObject param = params.add<JsonObject>();
            FastBee::setFixedJsonField(param, "key", parameter.name);
            FastBee::setFixedJsonField(param, "type", parameter.type);
            FastBee::setFixedJsonField(param, "unit", parameter.unit);
            FastBee::setFixedJsonField(param, "default", parameter.defaultValue);
            FastBee::setFixedJsonField(param, "min", parameter.minValue);
            FastBee::setFixedJsonField(param, "max", parameter.maxValue);
            FastBee::setFixedJsonField(param, "required", parameter.required);
            JsonArray choices = param["choices"].to<JsonArray>();
            for (uint8_t c = 0; c < parameter.choiceCount; ++c) {
                choices.add(parameter.choices[c]);
            }
        }
    }
}

void addIoExpanderDriverDescriptors(JsonObject typeObj) {
    const bool active = FastBee::RuntimeFeatureManager::instance().shouldEnable("ioExpander");
    FastBee::setFixedJsonField(typeObj, "featureActive", active);
    FastBee::setFixedJsonField(typeObj, "channelCount", 8);
    JsonArray names = typeObj["drivers"].to<JsonArray>();
    JsonArray descriptors = typeObj["driverDescriptors"].to<JsonArray>();
#if FASTBEE_ENABLE_IO_EXPANDER
    names.add("PCF8574");
    JsonObject descriptor = descriptors.add<JsonObject>();
    FastBee::setFixedJsonField(descriptor, "name", "PCF8574");
    FastBee::setFixedJsonField(descriptor, "busType", "i2c");
    FastBee::setFixedJsonField(descriptor, "compiled", true);
    FastBee::serializeIoProviderFields(descriptor, false);
    FastBee::setFixedJsonField(descriptor, "channelCount", 8);
    FastBee::setFixedJsonField(descriptor, "featureKey", "ioExpander");
    FastBee::setFixedJsonField(descriptor, "featureActive", active);
    descriptor["addresses"].to<JsonArray>();
    descriptor["channels"].to<JsonArray>();
    descriptor["params"].to<JsonArray>();
    names.add("MCP23017");
    JsonObject mcp = descriptors.add<JsonObject>();
    FastBee::setFixedJsonField(mcp, "name", "MCP23017");
    FastBee::setFixedJsonField(mcp, "busType", "i2c");
    FastBee::setFixedJsonField(mcp, "compiled", true);
    FastBee::serializeIoProviderFields(mcp, true);
    FastBee::setFixedJsonField(mcp, "channelCount", 16);
    FastBee::setFixedJsonField(mcp, "featureKey", "ioExpander");
    FastBee::setFixedJsonField(mcp, "featureActive", active);
    FastBee::setFixedJsonField(mcp, "requiresResetReference", true);
    FastBee::setFixedJsonField(mcp, "groupActivation", true);
    JsonArray outputOnly = mcp["outputOnlyChannels"].to<JsonArray>();
    outputOnly.add(7);
    outputOnly.add(15);
    mcp["addresses"].to<JsonArray>();
    mcp["channels"].to<JsonArray>();
    mcp["params"].to<JsonArray>();
#else
    (void)names;
    (void)descriptors;
#endif
}

void addScreenDisplayDescriptor(JsonObject typeObj) {
    FastBee::setFixedJsonField(typeObj, "minPinCount", 0);
    FastBee::setFixedJsonField(typeObj, "maxPinCount", 0);
    FastBee::setFixedJsonField(typeObj, "usesNamedPins", true);
    FastBee::setFixedJsonField(typeObj, "featureKey", "screenPanel");
    FastBee::setFixedJsonField(typeObj, "uiRuntimeCompiled", FASTBEE_ENABLE_SCREEN_UI != 0);
    JsonArray drivers = typeObj["driverOptions"].to<JsonArray>();
    struct ScreenDriver {
        const char* backend;
        const char* driver;
        const char* label;
        int minimum;
        int maximum;
        const char* rgbInitProfile;
    };
    static const ScreenDriver options[] = {
        {"spi_tft", "st7789", "ST7789 SPI TFT", 128, 320, nullptr},
        {"spi_tft", "ili9341", "ILI9341 SPI TFT", 128, 320, nullptr},
#if CHIP_IS_ESP32S3
        {"rgb", "st7701", "ST7701 RGB565 (type1)", 480, 480, "st7701_type1"},
#endif
    };
    for (const auto& option : options) {
        JsonObject driver = drivers.add<JsonObject>();
        FastBee::setFixedJsonField(driver, "backend", JsonString(option.backend, true));
        FastBee::setFixedJsonField(driver, "driver", JsonString(option.driver, true));
        FastBee::setFixedJsonField(driver, "label", JsonString(option.label, true));
        FastBee::setFixedJsonField(driver, "minWidth", option.minimum);
        FastBee::setFixedJsonField(driver, "maxWidth", option.maximum);
        FastBee::setFixedJsonField(driver, "minHeight", option.minimum);
        FastBee::setFixedJsonField(driver, "maxHeight", option.maximum);
        if (option.rgbInitProfile) {
            FastBee::setFixedJsonField(driver, "rgbInitProfile", JsonString(option.rgbInitProfile, true));
            FastBee::setFixedJsonField(driver, "requiresPsram", true);
        }
    }
    JsonArray touchOptions = typeObj["touchOptions"].to<JsonArray>();
    JsonObject noTouch = touchOptions.add<JsonObject>();
    FastBee::setFixedJsonField(noTouch, "backend", "none");
    FastBee::setFixedJsonField(noTouch, "label", "No touch");
    JsonObject gt911 = touchOptions.add<JsonObject>();
    FastBee::setFixedJsonField(gt911, "backend", "gt911");
    FastBee::setFixedJsonField(gt911, "label", "GT911 I2C");
    JsonArray addresses = gt911["addresses"].to<JsonArray>();
    addresses.add(0x5D);
    addresses.add(0x14);
    JsonArray requiredPins = typeObj["requiredNamedPins"].to<JsonArray>();
    requiredPins.add("mosi");
    requiredPins.add("sclk");
    requiredPins.add("cs");
    requiredPins.add("dc");
    JsonArray optionalPins = typeObj["optionalNamedPins"].to<JsonArray>();
    optionalPins.add("rst");
    optionalPins.add("backlight");
    optionalPins.add("touchSda");
    optionalPins.add("touchScl");
    optionalPins.add("touchInt");
    optionalPins.add("touchRst");
    JsonObject capabilities = typeObj["capabilities"].to<JsonObject>();
    FastBee::setFixedJsonField(capabilities, "colorDepth", 16);
    FastBee::setFixedJsonField(capabilities, "partialFlush", true);
    FastBee::setFixedJsonField(capabilities, "brightness", true);
    FastBee::setFixedJsonField(capabilities, "sleep", true);
    FastBee::setFixedJsonField(capabilities, "touch", true);
    FastBee::setFixedJsonField(capabilities, "singleActiveDisplay", true);
    JsonObject limits = typeObj["configLimits"].to<JsonObject>();
    static const struct { const char* key; int value; } limitFields[] = {
        {"widthMin", 128}, {"widthMax", 320}, {"heightMin", 128}, {"heightMax", 320},
        {"rotationMin", 0}, {"rotationMax", 3}, {"colorDepth", 16},
        {"bufferLinesMin", 1}, {"bufferLinesMax", 64},
        {"brightnessMin", 0}, {"brightnessMax", 100},
        {"spiFrequencyMin", 1000000}, {"spiFrequencyMax", 80000000},
        {"rgbFrequencyMin", 1000000}, {"rgbFrequencyMax", 16000000},
    };
    for (const auto& field : limitFields) {
        // These table keys also have static lifetime; preserve integer types.
        FastBee::fixedJsonFieldForWrite(limits, field.key).set(field.value);
    }
    JsonArray i2cFrequencies = limits["i2cFrequencies"].to<JsonArray>();
    i2cFrequencies.add(100000);
    i2cFrequencies.add(400000);
}

#if defined(__clang__)
__attribute__((noinline))
#elif defined(__GNUC__)
__attribute__((noinline, noclone))
#endif
void appendCatalogCopiedValues(JsonArray target, std::initializer_list<const char*> values) {
    // Pointer values intentionally keep the original copied-string policy.
    for (const char* value : values) target.add(value);
}

void addPeripheralTypeDescriptorDetails(JsonObject typeObj,
                                        PeripheralType type) {
    FastBee::setFixedJsonField(typeObj, "configurable", isPeripheralTypeUserConfigurable(type));
    if (type == PeripheralType::MODBUS_DEVICE) {
        FastBee::setFixedJsonField(typeObj, "runtimeImplemented", isPeripheralTypeImplemented(type));
        // Existing Web clients use `implemented` to decide whether a type can
        // be selected, so keep this protocol-owned target out of ordinary CRUD.
        FastBee::setFixedJsonField(typeObj, "implemented", false);
        FastBee::setFixedJsonField(typeObj, "runtimeManaged", true);
        FastBee::setFixedJsonField(typeObj, "owner", "protocol.modbus");
    }
    JsonArray roles = typeObj["pinRoles"].to<JsonArray>();
    auto addRoles = [&roles](std::initializer_list<const char*> values) {
        appendCatalogCopiedValues(roles, values);
    };

    switch (type) {
        case PeripheralType::GPIO_DIGITAL_OUTPUT:
            FastBee::writeThermostatConfigJson(typeObj["thermostatConfig"].to<JsonObject>());
            break;
        case PeripheralType::UART: addRoles({"RX", "TX"}); break;
        case PeripheralType::I2C: addRoles({"SDA", "SCL"}); break;
        case PeripheralType::IO_EXPANDER:
            addRoles({"SDA", "SCL"});
            FastBee::setFixedJsonField(typeObj, "minPinCount", 0);
            FastBee::setFixedJsonField(typeObj, "maxPinCount", 2);
            addIoExpanderDriverDescriptors(typeObj);
            break;
        case PeripheralType::PCA9685_PWM:
            FastBee::setFixedJsonField(typeObj, "minPinCount", 0); FastBee::setFixedJsonField(typeObj, "maxPinCount", 0);
            FastBee::setFixedJsonField(typeObj, "featureKey", "ioExpander");
            FastBee::setFixedJsonField(typeObj, "featureActive", FastBee::RuntimeFeatureManager::instance().shouldEnable("ioExpander"));
            FastBee::setFixedJsonField(typeObj, "sharedFrequency", true);
            FastBee::setFixedJsonField(typeObj, "requiresOeReference", true);
            FastBee::setFixedJsonField(typeObj, "requiresFreshPowerCycle", true);
            FastBee::setFixedJsonField(typeObj, "maxInstances", 1);
            FastBee::serializePca9685Fields(typeObj);
            break;
        case PeripheralType::SPI: addRoles({"MISO", "MOSI", "SCK", "CS"}); break;
        case PeripheralType::SDIO:
            addRoles({"SCK", "MOSI", "MISO", "CS"});
            FastBee::writeSdRecorderConfigJson(typeObj["sdRecorderConfig"].to<JsonObject>());
            break;
        case PeripheralType::SENSOR_GENERIC:
            // SENSOR_GENERIC covers single-wire/DHT, HC-SR04 and explicit
            // local I2C drivers. A single fixed SDA/SCL descriptor caused Web
            // templates to assign the wrong roles to DHT and ultrasonic pins.
            addRoles({"DATA"});
            FastBee::setFixedJsonField(typeObj, "minPinCount", 0);
            FastBee::setFixedJsonField(typeObj, "maxPinCount", 2);
            {
                JsonArray variants = typeObj["pinRoleVariants"].to<JsonArray>();
                JsonObject singleWire = variants.add<JsonObject>();
                FastBee::setFixedJsonField(singleWire, "key", "single-wire");
                singleWire["pinRoles"].to<JsonArray>().add("DATA");
                JsonObject ultrasonic = variants.add<JsonObject>();
                FastBee::setFixedJsonField(ultrasonic, "key", "ultrasonic");
                FastBee::setFixedJsonField(ultrasonic, "sensorType", 3);
                JsonArray ultrasonicRoles = ultrasonic["pinRoles"].to<JsonArray>();
                ultrasonicRoles.add("TRIG");
                ultrasonicRoles.add("ECHO");
                JsonObject i2cDriver = variants.add<JsonObject>();
                FastBee::setFixedJsonField(i2cDriver, "key", "i2c-driver");
                JsonArray drivers = i2cDriver["drivers"].to<JsonArray>();
                const FastBee::DriverRegistry& registry = FastBee::DriverRegistry::getInstance();
                for (size_t i = 0; i < registry.getDriverCount(); ++i) {
                    const FastBee::SensorDriverEntry* entry = registry.getDriverEntry(i);
                    if (entry && entry->metadata) drivers.add(entry->metadata->name);
                }
                JsonArray i2cRoles = i2cDriver["pinRoles"].to<JsonArray>();
                i2cRoles.add("SDA");
                i2cRoles.add("SCL");
            }
            break;
        case PeripheralType::SCREEN_DISPLAY:
            addScreenDisplayDescriptor(typeObj);
            break;
        case PeripheralType::PWM_SERVO: addRoles({"PWM"}); break;
        case PeripheralType::STEPPER_MOTOR:
            addRoles({"IN1", "IN2", "IN3", "IN4"});
            break;
        case PeripheralType::ENCODER: addRoles({"A", "B"}); break;
        case PeripheralType::ONE_WIRE:
        case PeripheralType::NEO_PIXEL:
        case PeripheralType::RF_MODULE:
        case PeripheralType::IR_RECEIVER:
            addRoles({"DATA"});
            break;
        case PeripheralType::SEVEN_SEGMENT_TM1637:
            addRoles({"CLK", "DIO"});
            break;
        case PeripheralType::RADAR_SENSOR: addRoles({"OUT"}); break;
        case PeripheralType::DS1302_RTC:
            addRoles({"CE", "IO", "SCLK"});
            break;
        case PeripheralType::LCD1602: addRoles({"SDA", "SCL"}); break;
        case PeripheralType::RFID_MFRC522:
            addRoles({"SCK", "MOSI", "MISO", "SS", "RST"});
            break;
        default:
            if (getPeripheralPinCount(type) == 1) addRoles({"GPIO"});
            break;
    }

    JsonArray operations = typeObj["operations"].to<JsonArray>();
    auto addOperations = [&operations](std::initializer_list<const char*> values) {
        appendCatalogCopiedValues(operations, values);
    };
    switch (type) {
        case PeripheralType::UART: addOperations({"read", "write"}); break;
        case PeripheralType::I2C: addOperations({"scan"}); break;
        case PeripheralType::SDIO:
            typeObj["supportedInterfaces"].to<JsonArray>().add("sdspi");
            addOperations({"status", "mount", "unmount", "list", "read-file",
                           "write-file", "delete-file"});
            break;
        case PeripheralType::SENSOR_GENERIC:
            addSensorDriverDescriptors(typeObj);
            addOperations({"scan", "probe", "read", "release"});
            break;
        case PeripheralType::DS1302_RTC:
            addOperations({"read", "set-datetime", "set-running"});
            break;
        case PeripheralType::LCD1602:
            addOperations({"status", "print", "clear", "backlight"});
            break;
        case PeripheralType::RFID_MFRC522:
            addOperations({"scan", "read-block", "write-block"});
            break;
        case PeripheralType::ENCODER:
            addOperations({"read", "reset"});
            break;
        case PeripheralType::PWM_SERVO:
            addOperations({"read", "set-angle"});
            break;
        default:
            break;
    }
}

// These descriptors contain only compile-independent type metadata. The
// full catalog regression compares every byte with the descriptor builders.
// Runtime/driver/feature fields continue through fresh owned serialization.
const char* catalogLiteralFragment(int value) {
    switch (value) {
        case 1: return "{\"value\":1,\"name\":\"UART\",\"pinCount\":2,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"RX\",\"TX\"],\"operations\":[\"read\",\"write\"]}";
        case 2: return ",{\"value\":2,\"name\":\"I2C\",\"pinCount\":2,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"SDA\",\"SCL\"],\"operations\":[\"scan\"]}";
        case 3: return ",{\"value\":3,\"name\":\"SPI\",\"pinCount\":4,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"MISO\",\"MOSI\",\"SCK\",\"CS\"],\"operations\":[]}";
        case 4: return ",{\"value\":4,\"name\":\"CAN\",\"pinCount\":8,\"implemented\":false,\"configurable\":false,\"pinRoles\":[],\"operations\":[]}";
        case 5: return ",{\"value\":5,\"name\":\"USB\",\"pinCount\":8,\"implemented\":false,\"configurable\":false,\"pinRoles\":[],\"operations\":[]}";
        case 11: return "],\"gpio\":[{\"value\":11,\"name\":\"Digital Input\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 13: return ",{\"value\":13,\"name\":\"Digital Input (Pull-up)\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 14: return ",{\"value\":14,\"name\":\"Digital Input (Pull-down)\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 15: return ",{\"value\":15,\"name\":\"Analog Input\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 16: return ",{\"value\":16,\"name\":\"Analog Output\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 17: return ",{\"value\":17,\"name\":\"PWM Output\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 18: return ",{\"value\":18,\"name\":\"Interrupt (Rising)\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 19: return ",{\"value\":19,\"name\":\"Interrupt (Falling)\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 20: return ",{\"value\":20,\"name\":\"Interrupt (Change)\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 21: return ",{\"value\":21,\"name\":\"Touch\",\"pinCount\":1,\"implemented\":false,\"configurable\":false,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 26: return "],\"analog\":[{\"value\":26,\"name\":\"ADC\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 27: return ",{\"value\":27,\"name\":\"DAC\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"GPIO\"],\"operations\":[]}";
        case 39: return ",{\"value\":39,\"name\":\"Camera\",\"pinCount\":8,\"implemented\":false,\"configurable\":false,\"pinRoles\":[],\"operations\":[]}";
        case 40: return ",{\"value\":40,\"name\":\"Ethernet\",\"pinCount\":8,\"implemented\":false,\"configurable\":false,\"pinRoles\":[],\"operations\":[]}";
        case 41: return ",{\"value\":41,\"name\":\"Servo\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"PWM\"],\"operations\":[\"read\",\"set-angle\"]}";
        case 42: return ",{\"value\":42,\"name\":\"Stepper Motor\",\"pinCount\":4,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"IN1\",\"IN2\",\"IN3\",\"IN4\"],\"operations\":[]}";
        case 43: return ",{\"value\":43,\"name\":\"Encoder\",\"pinCount\":2,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"A\",\"B\"],\"operations\":[\"read\",\"reset\"]}";
        case 44: return ",{\"value\":44,\"name\":\"OneWire\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"DATA\"],\"operations\":[]}";
        case 48: return ",{\"value\":48,\"name\":\"RF Module\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"DATA\"],\"operations\":[]}";
        case 49: return ",{\"value\":49,\"name\":\"Radar Sensor\",\"pinCount\":1,\"implemented\":true,\"configurable\":true,\"pinRoles\":[\"OUT\"],\"operations\":[]}";
        case 60: return ",{\"value\":60,\"name\":\"Device Event\",\"pinCount\":0,\"implemented\":true,\"configurable\":true,\"pinRoles\":[],\"operations\":[]}]}}";
        default: return nullptr;
    }
}

} // namespace

void PeripheralRouteHandler::handleGetPeripheralTypes(AsyncWebServerRequest* request) {
    if (!ctx->requireAuth(request)) return;
    if (HandlerUtils::checkLowMemory(request, 16384)) return;

    // Keep full descriptors, but never construct the complete catalog DOM.
    // The two largest records are built first, before accumulated serialized
    // records consume the remaining heap on devices without PSRAM.
    static constexpr uint8_t types[] = {
        1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21,
        26, 27, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45,
        47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60
    };
    constexpr size_t typeCount = sizeof(types) / sizeof(types[0]);
    std::unique_ptr<FastBee::JsonFragmentsResponse> response(
        new (std::nothrow) FastBee::JsonFragmentsResponse(
            typeCount + 2, FastBee::JsonFragmentsResponse::Storage::CatalogLiterals));
    auto doc = FastBee::makeJsonDocument();
    auto fail = [&]() {
        doc.clear();
        response.reset();
        HandlerUtils::sendJsonError(request, 503, "Peripheral catalog memory unavailable", 5);
    };
    if (!response || !response->allocated()) { fail(); return; }

    JsonObject profile = doc.to<JsonObject>();
    const int profileMax = static_cast<int>(FastBee::ResourceProfile::MAX_PERIPHERALS);
    const int profileUsed = static_cast<int>(PeripheralManager::getInstance().getPeripheralCount());
    FastBee::setFixedJsonField(profile, "name", FastBee::ResourceProfile::NAME);
    FastBee::setFixedJsonField(profile, "max", profileMax);
    FastBee::setFixedJsonField(profile, "used", profileUsed);
    FastBee::setFixedJsonField(profile, "remaining", std::max(0, profileMax - profileUsed));
    if (!response->set(0, doc, "{\"success\":true,\"profile\":", ",\"hardware\":")) {
        fail(); return;
    }
    doc.clear();
    PeripheralManager& pm = PeripheralManager::getInstance();

    JsonObject hardware = doc.to<JsonObject>();
    FastBee::setFixedJsonField(hardware, "chip", CHIP_NAME);
    FastBee::setFixedJsonField(hardware, "maxGpio", CHIP_MAX_GPIO);
    FastBee::setFixedJsonField(hardware, "pwmChannels", CHIP_MAX_PWM_CH);
    FastBee::setFixedJsonField(hardware, "hasDac", (bool)CHIP_HAS_DAC);
    FastBee::setFixedJsonField(hardware, "hasTouch", (bool)CHIP_HAS_TOUCH);
    JsonArray inputPins = hardware["inputPins"].to<JsonArray>();
    JsonArray outputPins = hardware["outputPins"].to<JsonArray>();
    JsonArray adcPins = hardware["adcPins"].to<JsonArray>();
    JsonArray dacPins = hardware["dacPins"].to<JsonArray>();
    JsonArray touchPins = hardware["touchPins"].to<JsonArray>();
    JsonArray reservedPins = hardware["reservedPins"].to<JsonArray>();
    for (uint8_t pin = 0; pin <= CHIP_MAX_GPIO; ++pin) {
        String pinError;
        if (pm.isReservedPin(pin)) reservedPins.add(pin);
        if (pm.validatePinForType(pin, PeripheralType::GPIO_DIGITAL_INPUT, pinError)) inputPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::GPIO_DIGITAL_OUTPUT, pinError)) outputPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::ADC, pinError)) adcPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::DAC, pinError)) dacPins.add(pin);
        pinError = "";
        if (pm.validatePinForType(pin, PeripheralType::GPIO_TOUCH, pinError)) touchPins.add(pin);
    }

    if (!response->set(1, doc, "", ",\"data\":{\"communication\":[")) {
        fail(); return;
    }
    doc.clear();
    auto addType = [&](size_t index) -> bool {
        const int value = types[index];
        if (response->canBorrowCatalogLiteral()) {
            const char* literal = catalogLiteralFragment(value);
            if (literal) return response->setCatalogLiteral(index + 2, literal) &&
                FastBee::MemoryBudget::dramFree() >= 8192 &&
                FastBee::MemoryBudget::dramLargest() >= 4096;
        }
        const PeripheralType type = static_cast<PeripheralType>(value);
        JsonObject typeObj = doc.to<JsonObject>();
        FastBee::setFixedJsonField(typeObj, "value", value);
        FastBee::setFixedJsonField(typeObj, "name", getPeripheralTypeName(type));
        FastBee::setFixedJsonField(typeObj, "pinCount", getPeripheralPinCount(type));
        FastBee::setFixedJsonField(typeObj, "implemented", isPeripheralTypeImplemented(type));
        addPeripheralTypeDescriptorDetails(typeObj, type);
        const char* prefix = index == 0 ? "" : index == 5 ? "],\"gpio\":["
            : index == 16 ? "],\"analog\":[" : index == 18 ? "],\"special\":[" : ",";
        const bool ok = response->set(index + 2, doc, prefix,
                                      index + 1 == typeCount ? "]}}" : "");
        doc.clear();
        return ok && (HandlerUtils::psramCanServeJson() ||
            (FastBee::MemoryBudget::dramFree() >= 8192 &&
             FastBee::MemoryBudget::dramLargest() >= 4096));
    };
    // SENSOR_GENERIC and SCREEN_DISPLAY contain nested driver metadata.
    if (!addType(20) || !addType(37)) { fail(); return; }
    for (size_t index = 0; index < typeCount; ++index) {
        if (index == 20 || index == 37) continue;
        if (!addType(index)) { fail(); return; }
    }
    HandlerUtils::sendWithClose(request, response.release());
}

