#include "core/BoardProfile.h"
#include "network/BoardProfileCatalog.h"
#include "legacy_board_profile.h"
#include "legacy_board_catalog.h"
#include <cassert>
#include <cstring>
#include <string>
#include <type_traits>

static_assert(sizeof(FastBee::BoardPinProfile) == 12, "expected compact native layout");
static_assert(sizeof(LegacyFastBee::BoardPinProfile) == 16, "oracle is original padded layout");
static_assert(std::is_standard_layout<FastBee::BoardPinProfile>::value, "native layout");
static_assert(std::is_trivially_copyable<FastBee::BoardPinProfile>::value, "plain value copy");
constexpr FastBee::BoardPinProfile zero{};
constexpr FastBee::BoardPinProfile partial{7};
constexpr FastBee::BoardPinProfile positional{7, 0xffff, FastBee::PinPolicy::CONDITIONAL, 0xffffffffU, 254, 255};
static_assert(zero.gpio == 0 && zero.capabilities == 0 && zero.risks == 0 &&
              zero.policy == FastBee::PinPolicy::RECOMMENDED &&
              zero.adcUnit == 0 && zero.adcChannel == 0, "value initialization");
static_assert(partial.gpio == 7 && partial.capabilities == 0 && partial.adcChannel == 0, "partial braces");
static_assert(positional.gpio == 7 && positional.capabilities == 0xffff &&
              positional.policy == FastBee::PinPolicy::CONDITIONAL && positional.risks == 0xffffffffU &&
              positional.adcUnit == 254 && positional.adcChannel == 255, "old order and full widths");

template<class A, class B> void comparePin(const A& a, const B& b, bool exhaustive) {
    assert(a.gpio == b.gpio);
    assert(a.capabilities == b.capabilities);
    assert(static_cast<unsigned>(a.policy) == static_cast<unsigned>(b.policy));
    assert(a.risks == b.risks);
    assert(a.adcUnit == b.adcUnit);
    assert(a.adcChannel == b.adcChannel);
    assert(std::strcmp(FastBee::pinPolicyCode(a.policy), LegacyFastBee::pinPolicyCode(b.policy)) == 0);
    // Every 16-bit requirement, including unknown/future capability bits.
    for (unsigned mask = 0; exhaustive && mask <= 0xffff; ++mask)
        assert(FastBee::boardPinHasCapabilities(a, static_cast<uint16_t>(mask)) ==
               LegacyFastBee::boardPinHasCapabilities(b, static_cast<uint16_t>(mask)));
}
void compareProfile(const FastBee::BoardProfile& a, const LegacyFastBee::BoardProfile& b, bool exhaustive = false) {
    assert(static_cast<unsigned>(a.id) == static_cast<unsigned>(b.id));
    assert(a.pinCount == b.pinCount);
#define TEXT(field) assert(std::strcmp(a.field, b.field) == 0)
    TEXT(key); TEXT(displayName); TEXT(version); TEXT(source);
#undef TEXT
#define VALUE(field) assert(a.field == b.field)
    VALUE(resources.i2cBusCount); VALUE(resources.spiBusCount); VALUE(resources.genericUartCount);
    VALUE(resources.firstGenericUartPort); VALUE(resources.ledcChannelCount); VALUE(resources.ledcTimerCount);
    VALUE(resources.rmtChannelCount); VALUE(resources.adcUnitCount); VALUE(resources.encoderSlotCount);
    VALUE(resources.pcntUnitCount); VALUE(resources.pcntChannelsPerUnit); VALUE(resources.sdmmcSupported);
    VALUE(defaults.i2cSda); VALUE(defaults.i2cScl); VALUE(defaults.spiMiso); VALUE(defaults.spiMosi);
    VALUE(defaults.spiSck); VALUE(defaults.spiCs); VALUE(defaults.uart0Rx); VALUE(defaults.uart0Tx);
    VALUE(defaults.onboardRgb);
#undef VALUE
    for (size_t i = 0; i < a.pinCount; ++i) {
        comparePin(a.pins[i], b.pins[i], exhaustive);
        assert(FastBee::findBoardPin(a, a.pins[i].gpio) == &a.pins[i]);
        assert(LegacyFastBee::findBoardPin(b, b.pins[i].gpio) == &b.pins[i]);
    }
    for (unsigned gpio = 0; gpio <= 255; ++gpio) {
        const auto* ap = FastBee::findBoardPin(a, static_cast<uint8_t>(gpio));
        const auto* bp = LegacyFastBee::findBoardPin(b, static_cast<uint8_t>(gpio));
        assert((ap == nullptr) == (bp == nullptr));
        if (ap) assert((ap - a.pins) == (bp - b.pins));
    }
}
template<class Catalog> std::string collect(Catalog& catalog, size_t chunk) {
    const size_t measured = catalog.measureLength();
    assert(catalog.valid() && measured > 10000);
    uint8_t bytes[1460];
    std::string result;
    result.reserve(measured);
    assert(catalog.read(nullptr, chunk) == 0 && catalog.read(bytes, 0) == 0);
    for (;;) {
        const size_t got = catalog.read(bytes, chunk);
        if (!got) break;
        result.append(reinterpret_cast<char*>(bytes), got);
    }
    assert(result.size() == measured && catalog.read(bytes, chunk) == 0);
    return result;
}
int main() {
    const FastBee::BoardProfile* current[] = {
        &FastBee::getPuzhongPzEsp32Profile(), &FastBee::getEsp32C6DevKitC1Profile(),
        &FastBee::getEsp32C3Profile(), &FastBee::getEsp32S3Profile(true),
        &FastBee::getEsp32S3Profile(false), &FastBee::getEsp32ModuleProfile(false),
        &FastBee::getEsp32ModuleProfile(true)};
    const LegacyFastBee::BoardProfile* legacy[] = {
        &LegacyFastBee::getPuzhongPzEsp32Profile(), &LegacyFastBee::getEsp32C6DevKitC1Profile(),
        &LegacyFastBee::getEsp32C3Profile(), &LegacyFastBee::getEsp32S3Profile(true),
        &LegacyFastBee::getEsp32S3Profile(false), &LegacyFastBee::getEsp32ModuleProfile(false),
        &LegacyFastBee::getEsp32ModuleProfile(true)};
    size_t pins = 0;
    for (size_t i = 0; i < 7; ++i) {
        compareProfile(*current[i], *legacy[i], true);
        pins += current[i]->pinCount;
    }
    assert(pins == 271);
    assert(pins * (sizeof(LegacyFastBee::BoardPinProfile) - sizeof(FastBee::BoardPinProfile)) == 1084);
    compareProfile(FastBee::getCompiledBoardProfile(), LegacyFastBee::getCompiledBoardProfile());
    for (unsigned id = 0; id <= 255; ++id)
        compareProfile(FastBee::getBoardProfile(static_cast<FastBee::BoardProfileId>(id)),
                       LegacyFastBee::getBoardProfile(static_cast<LegacyFastBee::BoardProfileId>(id)));
    const size_t chunks[] = {1, 7, 137, 1460};
    for (int active = -1; active < 7; ++active) {
        FastBee::BoardProfileCatalog fresh(active < 0 ? nullptr : current[active], "chip\"\\\x01");
        LegacyFastBee::BoardProfileCatalog original(active < 0 ? nullptr : legacy[active], "chip\"\\\x01");
        for (size_t chunk : chunks) assert(collect(fresh, chunk) == collect(original, chunk));
    }
    return 0;
}
