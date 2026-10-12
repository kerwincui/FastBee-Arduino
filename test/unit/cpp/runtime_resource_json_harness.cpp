#include <Arduino.h>
// This fixture injects failures through its own allocator. Avoid the general
// Arduino mock's malloc/free macros rewriting qualified std:: calls (including
// ArduinoJson's allocator), while keeping String's normal mock implementation.
#undef malloc
#undef free
#include "network/RuntimePinResourceJson.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
using namespace FastBee;

struct ScratchProbe {
    static inline bool fail = false;
    static inline unsigned alive = 0;
    char id[16]{};
    ScratchProbe() { ++alive; }
    ~ScratchProbe() { --alive; }
    static void* operator new(size_t bytes, const std::nothrow_t&) noexcept {
        return fail ? nullptr : std::malloc(bytes);
    }
    static void operator delete(void* pointer) noexcept { std::free(pointer); }
    static void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
};

class FieldProbeAllocator : public ArduinoJson::Allocator {
public:
    explicit FieldProbeAllocator(unsigned limit) : limit(limit) {}
    unsigned calls = 0, limit;
    void* allocate(size_t bytes) override {
        return calls++ >= limit ? nullptr : std::malloc(bytes);
    }
    void deallocate(void* pointer) override { std::free(pointer); }
    void* reallocate(void* pointer, size_t bytes) override {
        return calls++ >= limit ? nullptr : std::realloc(pointer, bytes);
    }
};

void populateFieldProbe(JsonDocument& doc, bool shared) {
    JsonObject object = doc.to<JsonObject>();
    char mutableText[] = "mutable text that must remain owned by the document";
    const char* pointerText = mutableText;
    String arduinoText(mutableText);
    auto allocatedRhs = [&object]() {
        object["rhsMarker"] = std::string(64, 'm');
        return String(std::string(96, 'r'));
    };
    if (shared) {
        setFixedJsonField(object, "flag", true);
        setFixedJsonField(object, "negative", int32_t(-2147483647 - 1));
        setFixedJsonField(object, "unsigned", uint32_t(0xffffffffu));
        setFixedJsonField(object, "fraction", 1.25);
        setFixedJsonField(object, "null", nullptr);
        setFixedJsonField(object, "literal", "permanent long literal text");
        setFixedJsonField(object, "mutable", mutableText);
        setFixedJsonField(object, "pointer", pointerText);
        setFixedJsonField(object, "arduinoString", arduinoText);
        setFixedJsonField(object, "explicitCopy", JsonString(mutableText, false));
        setFixedJsonField(object, "rhsText", allocatedRhs());
        setFixedJsonField(object, "overwrite", std::string(96, 'a'));
        setFixedJsonField(object, "overwrite", std::string(96, 'b'));
    } else {
        object["flag"] = true;
        object["negative"] = int32_t(-2147483647 - 1);
        object["unsigned"] = uint32_t(0xffffffffu);
        object["fraction"] = 1.25;
        object["null"] = nullptr;
        object["literal"] = "permanent long literal text";
        object["mutable"] = mutableText;
        object["pointer"] = pointerText;
        object["arduinoString"] = arduinoText;
        object["explicitCopy"] = JsonString(mutableText, false);
        object["rhsText"] = allocatedRhs();
        object["overwrite"] = std::string(96, 'a');
        object["overwrite"] = std::string(96, 'b');
    }
    mutableText[0] = 'X';
    arduinoText = "changed after serialization";
    if (!doc.overflowed()) {
        assert(object["flag"].is<bool>() && object["flag"] == true);
        assert(object["negative"] == int32_t(-2147483647 - 1));
        assert(object["unsigned"] == uint32_t(0xffffffffu));
        assert(object["fraction"] == 1.25 && object["null"].isNull());
        assert(object["literal"].as<JsonString>().isStatic());
        for (const char* key : {"mutable", "pointer", "arduinoString", "explicitCopy"}) {
            assert(object[key] == "mutable text that must remain owned by the document");
            assert(!object[key].as<JsonString>().isStatic());
        }
    }
}

void checkFixedFieldContract() {
    JsonDocument preserved;
    auto object = preserved.to<JsonObject>();
    object["existing"] = "retain the current value until a new set";
    const auto existing = fixedJsonFieldForWrite(object, "existing");
    assert(existing == "retain the current value until a new set");
    object["nested"]["child"] = 17;
    const auto nested = fixedJsonFieldForWrite(object, "nested");
    assert(nested["child"] == 17); // lookup alone must not clear an existing object
    static const char borrowedKey[] = "borrowedStaticKey";
    assert(setFixedJsonField(object, borrowedKey, 41));
    bool found = false;
    for (JsonPair pair : object) {
        if (pair.key() == borrowedKey) {
            found = true;
            assert(pair.key().c_str() == borrowedKey && pair.key().isStatic());
        }
    }
    assert(found);
    assert(!setFixedJsonField(JsonObject(), "unbound", true));

    bool failed = false, complete = false;
    for (unsigned limit = 0; limit < 128; ++limit) {
        FieldProbeAllocator baselineAllocator(limit), sharedAllocator(limit);
        JsonDocument baseline(&baselineAllocator), actual(&sharedAllocator);
        populateFieldProbe(baseline, false);
        populateFieldProbe(actual, true);
        std::string expectedBytes, actualBytes;
        serializeJson(baseline, expectedBytes);
        serializeJson(actual, actualBytes);
        assert(actualBytes == expectedBytes);
        assert(actual.overflowed() == baseline.overflowed());
        assert(sharedAllocator.calls == baselineAllocator.calls);
        if (actual.overflowed()) failed = true;
        else {
            complete = true;
            const auto marker = actualBytes.find("\"rhsMarker\"");
            const auto value = actualBytes.find("\"rhsText\"");
            assert(marker != std::string::npos && marker < value);
            break;
        }
    }
    assert(failed && complete);
}

#include "fixed_json_forwarding_checks.h"

int main() {
    FixedJsonForwardingTest::run();
    checkFixedFieldContract();
    JsonDocument doc;
    JsonObject runtime = doc.to<JsonObject>();
    writeRuntimeResourceHeader(runtime, "active-board");
    assert(runtime["status"] == "partial" && runtime["scope"] == "managed_leases");
    assert(runtime["complete"] == false && runtime["hardwareProbed"] == false);
    assert(runtime["freeCapacityKnown"] == false && runtime["atomicAcrossSources"] == false);
    assert(runtime["activeProfileId"] == "active-board");
    assert(runtime["unavailable"].size() == 5 && runtime["unavailable"][0]["kind"] == "LEDC");
    assert(runtime["unavailable"][3]["status"] == "unsupported");
    assert(runtime["unavailable"][0]["reason"] == "PARTIAL_PM_SCRIPT_AND_BACKLIGHT_COVERAGE");
    JsonObject data = runtime["source"].to<JsonObject>();
    I2CResourceSnapshot i2c;
    i2c.count = 16; i2c.initialized = false; i2c.sda = 4; i2c.scl = 5;
    for (uint8_t i = 0; i < 16; ++i) i2c.leases[i] = {i, static_cast<uint8_t>(32 + i), i == 15};
    writeI2CResourceJson(data, i2c);
    assert(data["leaseCount"] == 16 && data["leases"].size() == 16);
    assert(data["busState"] == "retained_uninitialized" && data["leases"][15]["cleanupPending"] == true);
    assert(data["leases"][0]["ownerKnown"] == false && data["leases"][0]["ownerId"].isNull());
    assert(i2c.leases[0].consumer.assign(ResourceConsumerRole::Sensor, "soil-sensor"));
    writeI2CResourceJson(data, i2c);
    assert(data["leases"][0]["ownerId"] == "soil-sensor" && data["leases"][0]["ownerRole"] == "sensor");
    i2c.leases[0].consumer.id[0]='X';
    assert(data["leases"][0]["ownerId"] == "soil-sensor");
    i2c.status = ResourceObservation::Busy;
    writeI2CResourceJson(data, i2c);
    assert(data["status"] == "busy" && data["leaseCount"].isNull() && data["leases"].isNull());
    assert(data["sda"].isNull() && data["managerInitialized"].isNull()); // No stale observed values.
    i2c = I2CResourceSnapshot{};
    writeI2CResourceJson(data, i2c);
    assert(data["leaseCount"] == 0 && data["leases"].is<JsonArray>() && data["leases"].size() == 0);

    SpiResourceSnapshot spi;
    spi.initialized = true; spi.count = 2;
    std::strcpy(spi.leases[0].name, "sensor\"\\line\n");
    spi.leases[1].networkOwner = true;
    assert(spi.leases[1].consumer.assign(ResourceConsumerRole::Ethernet));
    spi.networkReserved = true;
    writeSpiResourceJson(data, spi);
    assert(data["leaseCount"] == 2 && data["network"]["includedInClientLeases"] == true);
    assert(data["leases"][0]["ownerId"] == "sensor\"\\line\n");
    assert(data["leases"][1]["ownerRole"] == "ethernet" && data["leases"][1]["ownerKnown"] == false);
    spi.leases[0].name[0] = 'X'; // JSON owns the copied ID past snapshot lifetime/reuse.
    assert(data["leases"][0]["ownerId"] == "sensor\"\\line\n");
    std::string text; serializeJson(doc, text);
    JsonDocument parsed; assert(!deserializeJson(parsed, text));
    assert(parsed["source"]["leases"][0]["ownerId"] == "sensor\"\\line\n");
    spi.networkDedicatedHost = true; spi.count = 1;
    writeSpiResourceJson(data, spi);
    assert(data["leaseCount"] == 2 && data["network"]["includedInClientLeases"] == false);
    spi.count = 0; spi.networkReserved = false;
    writeSpiResourceJson(data, spi);
    assert(data["leaseCount"] == 0 && data["retainedWithoutClients"] == true);
    assert(data["cleanupPending"].isNull() && data["cleanupReason"] == "LEASE_LIFECYCLE_NOT_RECORDED");
    spi.status = ResourceObservation::Busy;
    writeSpiResourceJson(data, spi);
    assert(data["leaseCount"].isNull() && data["network"].isNull());

    UartResourceSnapshot uart;
    uart.portCount = 2; uart.generations[0] = 123; uart.generations[1] = 456; uart.generations[2] = 789;
    assert(uart.consumers[1].assign(ResourceConsumerRole::Peripheral,"serial-sensor"));
    writeUartResourceJson(data, uart);
    assert(data["leaseCount"] == 1 && data["leases"].size() == 1);
    assert(data["leases"][0]["port"] == 1 && data["leases"][0]["generation"] == 456);
    assert(data["leases"][0]["ownerId"] == "serial-sensor");
    assert(data["cleanupPending"].isNull() && data["atomicAcrossPorts"] == false);
    uart.portCount = 3;
    writeUartResourceJson(data, uart);
    assert(data["leaseCount"] == 2);

    ControlOutputResourceSnapshot controls;
    controls.count = 4;
    for (uint8_t i = 0; i < 4; ++i) {
        controls.leases[i].slot = i; controls.leases[i].pin = 4 + i;
        std::strcpy(controls.leases[i].id, "control");
    }
    controls.leases[0].fault = true;
    controls.leases[1].retiring = true;
    controls.leases[2].retiring = true; controls.leases[2].fault = true;
    controls.leases[2].retirementAttempted = true;
    writeControlOutputResourceJson(data, controls);
    assert(data["leaseCount"] == 4 && data["leases"].size() == 4);
    assert(data["leases"][0]["state"] == "retained_after_fault" && data["leases"][0]["cleanupFailed"] == false);
    assert(data["leases"][1]["state"] == "retiring" && data["leases"][1]["cleanupFailed"] == false);
    assert(data["leases"][2]["cleanupFailed"] == true);
    controls.leases[0].id[0] = 'X';
    assert(data["leases"][0]["resourceId"] == "control" && data["leases"][0]["ownerKnown"] == false);
    controls.status = ResourceObservation::Busy;
    writeControlOutputResourceJson(data, controls);
    assert(data["leases"].isNull() && data["leaseCount"].isNull());
    controls.status = ResourceObservation::Observed;
    assert(controls.leases[0].consumer.assign(ResourceConsumerRole::PcaGate, "pwm-consumer"));
    writeControlOutputResourceJson(data, controls);
    assert(data["leases"][0]["ownerId"] == "pwm-consumer" && data["leases"][0]["ownerRole"] == "pca9685_oe");
    controls.leases[0].consumer.id[0] = 'X';
    assert(data["leases"][0]["ownerId"] == "pwm-consumer");
    EncoderResourceSnapshot encoder; encoder.count = 4;
    std::strcpy(encoder.leases[0].id, "encoder"); encoder.leases[0].pinA=4; encoder.leases[0].pinB=5;
    encoder.leases[0].active=true; encoder.leases[0].interruptAttached=true;
    encoder.leases[1].cleanupPending=true; encoder.leases[1].interruptCleanupPending=true; encoder.leases[1].fault=true;
    writeEncoderResourceJson(data, encoder);
    assert(data["scope"] == "gpio_isr_slots" && data["pcntUsed"] == false && data["hardwareVerified"] == false);
    assert(data["interruptEvidence"] == "checked_sdk_lifecycle" && data["interruptAttachmentVerified"].isNull());
    assert(data["leaseCount"] == 4 && data["leases"][0]["ownerId"] == "encoder");
    assert(data["leases"][0]["active"] == true && data["leases"][0]["interruptAttached"] == true);
    assert(data["leases"][1]["active"] == false && data["leases"][1]["state"] == "retiring");
    assert(data["leases"][1]["interruptCleanupPending"] == true && data["leases"][1]["fault"] == true);
    encoder.status = ResourceObservation::Busy; writeEncoderResourceJson(data, encoder);
    assert(data["leases"].isNull() && data["leaseCount"].isNull());
    NeoPixelResourceSnapshot neo;
    writeNeoPixelResourceJson(data, neo);
    assert(data["status"] == "unsupported" && data["leaseCount"].isNull());
    neo.supported=true; neo.managerInitialized=true; neo.objectAllocated=true;
    neo.pin=18; neo.ledCount=32; neo.lifecycle=FeatureInstanceState::Allocated;
    assert(neo.consumer.assign(ResourceConsumerRole::Peripheral, "rgb-owner"));
    writeNeoPixelResourceJson(data, neo);
    assert(data["leaseCount"] == 1 && data["leases"][0]["ownerId"] == "rgb-owner");
    assert(data["rmtChannelCount"].isNull() && data["rmtCapacityKnown"] == false);
    assert(data["leases"][0]["state"] == "allocated" && data["leases"][0]["configuredLedCount"] == 32);
    assert(data["leases"][0]["lastWriteResult"].isNull() && data["leases"][0]["lastReleaseResult"].isNull());
    neo.consumer.id[0]='X'; assert(data["leases"][0]["ownerId"] == "rgb-owner");
    neo.lifecycle=FeatureInstanceState::CleanupPending; neo.projectPinRetained=true;
    neo.writeAttempted=true; neo.lastWriteResult=FASTBEE_NEO_NO_MEMORY;
    neo.releaseAttempted=true; neo.lastReleaseResult=FASTBEE_NEO_RELEASE_FAILED;
    writeNeoPixelResourceJson(data, neo);
    assert(data["leases"][0]["cleanupPending"] == true && data["leases"][0]["backendPinRetained"] == false);
    assert(data["leases"][0]["lastWriteResult"] == int(FASTBEE_NEO_NO_MEMORY));
    assert(data["leases"][0]["lastReleaseResult"] == int(FASTBEE_NEO_RELEASE_FAILED));
    neo.status=ResourceObservation::Busy; writeNeoPixelResourceJson(data, neo);
    assert(data["leases"].isNull() && data["objectAllocated"].isNull());
    assert(data["managerInitialized"] == true && data["reason"] == "REGISTRY_BUSY");
    neo.managerInitialized=false; writeNeoPixelResourceJson(data, neo);
    assert(data["managerInitialized"] == false && data["reason"] == "PM_SYNCHRONIZATION_UNAVAILABLE");
    assert(data["leaseCount"].isNull());
    neo=NeoPixelResourceSnapshot{}; neo.supported=true; writeNeoPixelResourceJson(data, neo);
    assert(data["leaseCount"] == 0 && data["leases"].size() == 0 && data["capacity"] == 1);
    StepperResourceSnapshot stepper; stepper.count=16; stepper.truncated=true;
    auto& phase=stepper.leases[0]; phase.consumer.assign(ResourceConsumerRole::Peripheral,"stepper-owner");
    phase.pins[0]=4; phase.pins[1]=5; phase.pins[2]=6; phase.pins[3]=7;
    phase.retainedMask=5; phase.gpioOwnedMask=5; phase.gpioReadyMask=4;
    phase.gpioFaultMask=1; phase.gpioCleanupPendingMask=1; phase.fault=true; phase.cleanupPending=true;
    writeStepperResourceJson(data, stepper);
    assert(data["leaseCount"] == 16 && data["truncated"] == true && data["gpioIsSublease"] == true);
    assert(data["leases"][0]["ownerId"] == "stepper-owner" && data["leases"][0]["pins"][3] == 7);
    assert(data["leases"][0]["retainedMask"] == 5 && data["leases"][0]["gpioCleanupPendingMask"] == 1);
    assert(data["leases"][0]["state"] == "retiring" && data["hardwareVerified"] == false);
    phase.consumer.id[0]='X'; assert(data["leases"][0]["ownerId"] == "stepper-owner");
    stepper.status=ResourceObservation::Busy; writeStepperResourceJson(data, stepper);
    assert(data["leases"].isNull() && data["truncated"].isNull());
    stepper=StepperResourceSnapshot{}; writeStepperResourceJson(data, stepper);
    assert(data["leaseCount"] == 0 && data["truncated"] == false);
    DhtRmtResourceSnapshot dht; writeDhtRmtResourceJson(data, dht);
    assert(data["status"] == "unsupported" && data["leaseCount"].isNull());
    dht.supported=true; writeDhtRmtResourceJson(data, dht);
    assert(data["scope"] == "dht_capture_only" && data["complete"] == false && data["leaseCount"] == 0);
    dht.retainedPin=15; writeDhtRmtResourceJson(data, dht);
    assert(data["leases"][0]["cleanupFailed"] == true && data["leases"][0]["channel"].isNull());
    dht.status=ResourceObservation::Busy; writeDhtRmtResourceJson(data, dht);
    assert(data["leases"].isNull() && data["leaseCount"].isNull());
    assert(!doc.overflowed());
    LedcResourceSnapshot ledc; ledc.count=16;
    std::strcpy(ledc.leases[0].id,"servo");ledc.leases[0].pin=18;ledc.leases[0].channel=2;
    ledc.leases[0].cleanupFailed=true;
    writeLedcResourceJson(data,ledc);
    assert(data["scope"] == "pm_and_script_lifecycles_only" && data["complete"] == false);
    assert(data["leaseCount"] == 16 && data["leases"][0]["ownerId"] == "servo");
    assert(data["leases"][0]["cleanupFailed"] == true && data["leases"][0]["lastAttachSucceeded"] == true);
    ledc.leases[0].id[0]='X';assert(data["leases"][0]["ownerId"] == "servo");
    ledc.leases[0].id[0]='\0';ledc.leases[0].commandScript=true;ledc.leases[0].channel=255;
    writeLedcResourceJson(data,ledc);
    assert(data["leases"][0]["ownerKnown"] == false && data["leases"][0]["ownerRole"] == "command_script");
    assert(data["leases"][0]["channel"].isNull());
    ledc.status=ResourceObservation::Busy;writeLedcResourceJson(data,ledc);
    assert(data["leaseCount"].isNull() && data["leases"].isNull());
    BacklightResourceSnapshot backlight;writeBacklightResourceJson(data,backlight,0);
    assert(data["status"] == "unknown" && data["leaseCount"].isNull());
    backlight.available=true;backlight.retained=true;backlight.cleanupFailed=true;
    backlight.pin=45;backlight.channel=3;backlight.sampledAt=UINT32_MAX-9;
    writeBacklightResourceJson(data,backlight,5);
    assert(data["sampleAgeMs"] == 15 && data["leases"][0]["cleanupFailed"] == true);
    assert(data["leases"][0]["ownerRole"] == "display_backlight");
    backlight.status=ResourceObservation::Busy;writeBacklightResourceJson(data,backlight,6);
    assert(data["leaseCount"].isNull() && data["leases"].isNull() && data["sampleAgeMs"].isNull());

    GpioResourceSnapshot gpio; gpio.count = GpioResourceSnapshot::CAPACITY;
    for (uint8_t i = 0; i < gpio.count; ++i) {
        gpio.leases[i].pin = i;
        gpio.leases[i].consumer.assign(ResourceConsumerRole::Peripheral, "input-owner");
    }
    gpio.leases[0].mode = ManagedGpioMode::PullUp;
    gpio.leases[0].interruptAttached = true; gpio.leases[0].interruptType = 3;
    gpio.leases[1].fault = true; gpio.leases[1].cleanupPending = true;
    gpio.leases[1].interruptCleanupPending = true;
    gpio.leases[2].consumer = {}; gpio.leases[2].commandScript = true;
    writeGpioResourceJson(data, gpio);
    assert(data["leases"].size() == GpioResourceSnapshot::CAPACITY && data["leaseCount"] == gpio.count);
    assert(data["controlOutputIsSublease"] == true && data["hardwareVerified"] == false);
    assert(data["leases"][0]["mode"] == "input_pullup" && data["leases"][0]["interruptType"] == 3);
    assert(data["leases"][1]["interruptType"].isNull() && data["leases"][1]["state"] == "retiring");
    assert(data["leases"][1]["interruptAttached"] == false && data["leases"][1]["interruptCleanupPending"] == true);
    assert(data["leases"][1]["cleanupFailed"].isNull()); // A retained fault is not a fabricated cleanup result.
    assert(data["leases"][2]["ownerRole"] == "command_script" && data["leases"][2]["ownerKnown"] == false);
    gpio.leases[0].consumer.id[0] = 'X';
    assert(data["leases"][0]["ownerId"] == "input-owner");
    gpio.status = ResourceObservation::Busy; writeGpioResourceJson(data, gpio);
    assert(data["leases"].isNull() && data["controlOutputIsSublease"].isNull());

    DacResourceSnapshot dac; writeDacResourceJson(data, dac);
    assert(data["status"] == "unsupported" && data["leaseCount"].isNull());
    dac.supported = true; dac.count = DacResourceSnapshot::CAPACITY;
    dac.leases[0].pin = 25; dac.leases[0].fault = true;
    dac.leases[0].consumer.assign(ResourceConsumerRole::Peripheral, "dac-output");
    writeDacResourceJson(data, dac);
    assert(data["leases"].size() == 2 && data["leases"][0]["state"] == "retained_after_fault");
    assert(data["leases"][0]["ownerId"] == "dac-output");
    dac.status = ResourceObservation::Busy; writeDacResourceJson(data, dac);
    assert(data["leases"].isNull() && data["leaseCount"].isNull());

    AdcResourceSnapshot adc; writeAdcResourceJson(data, adc);
    assert(data["status"] == "busy" && data["leases"].isNull());
    adc.observed = true; adc.initialized = true;
    adc.consumerCount = AdcResourceSnapshot::CONSUMER_CAPACITY;
    adc.unitCount = AdcResourceSnapshot::UNIT_CAPACITY;
    adc.channelCount = AdcResourceSnapshot::CHANNEL_CAPACITY;
    adc.calibrationCount = 2; adc.calibrationCleanupPending = 1;
    adc.units[0] = {0, true, true, -1};
    adc.channels[0].unit = 0; adc.channels[0].channel = 1; adc.channels[0].pin = 4;
    adc.channels[0].attenuation = 3; adc.channels[0].configurationKnown = true;
    adc.consumers[0].identity.assign(ResourceConsumerRole::Peripheral, "analog-input");
    adc.consumers[0].pin = 4; adc.consumers[0].resolution = 9;
    adc.consumers[0].retiring = true; adc.consumers[0].lastError = -2;
    adc.consumers[1].identity.assign(ResourceConsumerRole::Sensor);
    adc.consumers[1].transient = true;
    writeAdcResourceJson(data, adc);
    assert(data["leases"].size() == AdcResourceSnapshot::CONSUMER_CAPACITY);
    assert(data["units"].size() == 2 && data["units"][0]["cleanupFailed"] == true);
    assert(data["channels"].size() == AdcResourceSnapshot::CHANNEL_CAPACITY);
    assert(data["channels"][0]["configurationKnown"] == true && data["calibrationCleanupPending"] == 1);
    assert(data["channels"][0]["pin"] == 4);
    assert(data["leases"][0]["resolutionBits"] == 9 && data["hardwareResolutionBits"] == 12);
    assert(data["leases"][0]["state"] == "retiring" && data["leases"][0]["lastError"] == -2);
    assert(data["leases"][1]["ownerKnown"] == false && data["leases"][1]["transient"] == true);
    adc.consumers[0].identity.id[0] = 'X';
    assert(data["leases"][0]["ownerId"] == "analog-input");
    adc.consumerCount = 0; writeAdcResourceJson(data, adc);
    assert(data["leaseCount"] == 0 && data["units"][0]["retained"] == true);
    assert(data["individualChannelReleaseSupported"] == false && data["complete"] == false);
    adc.observed = false; writeAdcResourceJson(data, adc);
    assert(data["leases"].isNull() && data["units"].isNull() && data["calibrationCount"].isNull());

    unsigned observations = 0, serializations = 0;
    const auto observe = [&observations](ScratchProbe& probe) {
        assert(ScratchProbe::alive == 1); ++observations; std::strcpy(probe.id, "copied-owner");
    };
    const auto serialize = [&serializations](JsonObject value, const ScratchProbe& probe) {
        assert(ScratchProbe::alive == 1); ++serializations;
        value.clear(); value["ownerId"] = JsonString(probe.id, false);
    };
    writeAllocatedResourceSnapshot<ScratchProbe>(data, observe, serialize);
    assert(ScratchProbe::alive == 0 && observations == 1 && serializations == 1);
    assert(data["ownerId"] == "copied-owner");
    ScratchProbe::fail = true;
    writeAllocatedResourceSnapshot<ScratchProbe>(data, observe, serialize);
    assert(ScratchProbe::alive == 0 && observations == 1 && serializations == 1);
    assert(data["status"] == "unknown" && data["reason"] == "SNAPSHOT_ALLOCATION_FAILED");
    assert(data["leases"].isNull() && data["leaseCount"].isNull() && data["ownerId"].isNull());
}
