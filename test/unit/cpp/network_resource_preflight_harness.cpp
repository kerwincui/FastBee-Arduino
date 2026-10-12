#include <cassert>
#include <math.h>
#include "core/PeripheralConfig.h"
#include "core/PeripheralResourcePlanner.h"
using namespace FastBee;
PeripheralConfig spi(const char* id, uint8_t miso, uint8_t mosi, uint8_t sck, uint8_t cs) {
    PeripheralConfig c; c.id=id; c.type=PeripheralType::SPI; c.enabled=true;
    c.pinCount=4; c.pins[0]=miso; c.pins[1]=mosi; c.pins[2]=sck; c.pins[3]=cs;
    c.params.spi.frequency=1000000; return c;
}
PeripheralConfig gpio(uint8_t pin, bool enabled=true) {
    PeripheralConfig c; c.id="gpio"; c.type=PeripheralType::GPIO_DIGITAL_OUTPUT;
    c.enabled=enabled; c.pinCount=1; c.pins[0]=pin; return c;
}
PreflightContext context() {
    PreflightContext c; c.uart0ConsoleActive=false; c.usbSerialJtagActive=false;
    c.acknowledgements=ACK_STRAPPING_PIN|ACK_BOARD_SHARED_PIN|ACK_UART0_CONSOLE|ACK_ADC2_WITH_WIFI;
    return c;
}
int main() {
    const auto& s3=getEsp32S3Profile(false);
    auto c=context(); c.ethernet={true,13,11,12,10,9,14};
    PreflightResult result;
    auto bus=spi("spi",13,11,12,8);
    preflightPeripheralChange(s3,nullptr,0,bus,PreflightOperation::ADD,"",c,result);
    assert(result.allowed && result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));
    for (uint8_t pin : {9,10,11,12,13,14}) {
        auto output=gpio(pin);
        preflightPeripheralChange(s3,nullptr,0,output,PreflightOperation::ADD,"",c,result);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
        output.enabled=false;
        preflightPeripheralChange(s3,nullptr,0,output,PreflightOperation::ADD,"",c,result);
        assert(result.allowed && result.hasIssue(PreflightIssueCode::DORMANT_GPIO_CONFLICT));
        preflightPeripheralChange(s3,nullptr,0,output,PreflightOperation::ENABLE,"",c,result);
        assert(!result.allowed);
    }
    bus.pins[3]=10;
    preflightPeripheralChange(s3,nullptr,0,bus,PreflightOperation::ADD,"",c,result);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::SPI_CS_CONFLICT));
    bus.pins[3]=8; bus.pins[2]=7;
    preflightPeripheralChange(s3,nullptr,0,bus,PreflightOperation::ADD,"",c,result);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::SPI_BUS_PINS_MISMATCH));
    // Network-side candidate validation uses the same existing graph.
    auto existing=gpio(9); auto noNetwork=context();
    PeripheralResourcePlanner network(s3,noNetwork,result,&existing,1);
    network.addPeripheral(existing,ClaimState::ACTIVE);
    result.resetIssuesPreservingClaims(); network.addEthernet(c.ethernet);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
    // Invalid/duplicate/optional pins never manufacture successful claims.
    for (int mode=0; mode<4; ++mode) {
        auto bad=c.ethernet;
        if(mode==0) bad.mosi=-1;
        if(mode==1) bad.rst=-2;
        if(mode==2) bad.cs=bad.sck;
        if(mode==3) bad.mosi=26; // S3 flash pin.
        PeripheralResourcePlanner p(s3,noNetwork,result); p.addEthernet(bad);
        assert(!result.allowed);
    }
    auto optional=c.ethernet; optional.rst=-1; optional.irq=-1;
    PeripheralResourcePlanner valid(s3,noNetwork,result); valid.addEthernet(optional);
    assert(result.allowed);
    auto classic=context(); classic.ethernet={true,19,23,18,5,21,22};
    auto other=spi("separate",25,26,27,32);
    preflightPeripheralChange(getPuzhongPzEsp32Profile(),nullptr,0,other,PreflightOperation::ADD,"",classic,result);
    assert(result.allowed);
    other.pins[2]=18;
    preflightPeripheralChange(getPuzhongPzEsp32Profile(),nullptr,0,other,PreflightOperation::ADD,"",classic,result);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
    // GPIO identity is physical: unit is a sharing domain, not a second pin.
    PeripheralResourcePlanner crossHost(s3,noNetwork,result);
    ResourceClaim a; a.kind=ResourceKind::GPIO; a.state=ClaimState::ACTIVE;
    a.index=7; a.unit=1; a.ownerId="host1"; crossHost.addClaim(a);
    a.unit=0; a.ownerId="host0"; crossHost.addClaim(a);
    assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
    // Keep a value snapshot, including when caller passed a temporary context.
    auto saved=context(); saved.acknowledgements=ACK_NONE;
    PeripheralResourcePlanner frozen(getPuzhongPzEsp32Profile(),saved,result);
    saved.acknowledgements=ACK_STRAPPING_PIN;
    auto strap=gpio(5); frozen.addPeripheral(strap,ClaimState::ACTIVE);
    assert(result.hasIssue(PreflightIssueCode::PIN_STRAPPING_ACK_REQUIRED));
    for (const auto* profile : {&getEsp32C3Profile(), &getEsp32C6DevKitC1Profile()}) {
        uint8_t pins[7]{}; unsigned count=0;
        for(size_t i=0;i<profile->pinCount && count<7;++i) {
            const auto& pin=profile->pins[i];
            if(pin.policy != PinPolicy::HARD_FORBIDDEN &&
               boardPinHasCapabilities(pin, PIN_CAP_INPUT|PIN_CAP_OUTPUT)) pins[count++]=pin.gpio;
        }
        assert(count==7);
        auto ctx=context();
        ctx.ethernet={true,static_cast<int8_t>(pins[0]),static_cast<int8_t>(pins[1]),
            static_cast<int8_t>(pins[2]),static_cast<int8_t>(pins[3]),
            static_cast<int8_t>(pins[4]),static_cast<int8_t>(pins[5])};
        auto shared=spi("shared",pins[0],pins[1],pins[2],pins[6]);
        preflightPeripheralChange(*profile,nullptr,0,shared,PreflightOperation::ADD,"",ctx,result);
        assert(result.allowed && result.hasIssue(PreflightIssueCode::RESOURCE_SHARED));
        auto occupied=gpio(pins[4]);
        preflightPeripheralChange(*profile,nullptr,0,occupied,PreflightOperation::ADD,"",ctx,result);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
    }
}
