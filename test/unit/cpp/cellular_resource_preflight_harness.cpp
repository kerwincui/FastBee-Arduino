#include <cassert>
#include <cstring>
#include <math.h>
#include "core/PeripheralConfig.h"
#include "core/PeripheralResourcePlanner.h"
#include "network/CellularUartPort.h"
using namespace FastBee;
int main() {
    static_assert(cellularUartPort(0)==UINT8_MAX && cellularUartPort(1)==UINT8_MAX, "No UART0");
    static_assert(cellularUartPort(2)==1 && cellularUartPort(3)==2, "Fixed modem port");
    for (const auto* profile : {&getPuzhongPzEsp32Profile(), &getEsp32S3Profile(false),
                               &getEsp32S3Profile(true), &getEsp32C3Profile(), &getEsp32C6DevKitC1Profile()}) {
        uint8_t pins[7]{}; size_t found=0;
        for (size_t i=0; i<profile->pinCount && found<7; ++i) {
            const auto& pin=profile->pins[i];
            if (pin.policy!=PinPolicy::HARD_FORBIDDEN && boardPinHasCapabilities(pin,PIN_CAP_INPUT|PIN_CAP_OUTPUT))
                pins[found++]=pin.gpio;
        }
        assert(found==7);
        PreflightContext context;
        context.wifiActive=false; context.uart0ConsoleActive=false; context.usbSerialJtagActive=false;
        context.acknowledgements=ACK_STRAPPING_PIN|ACK_BOARD_SHARED_PIN|ACK_UART0_CONSOLE|ACK_ADC2_WITH_WIFI;
        context.cellular={true,static_cast<int8_t>(pins[0]),static_cast<int8_t>(pins[1]),static_cast<int8_t>(pins[2])};
        PreflightResult result;
        PeripheralResourcePlanner initial(*profile,context,result);
        assert(result.allowed && result.claimCount==4);
        const auto& uart=result.claims[0];
        assert(uart.kind==ResourceKind::UART_PORT && uart.state==ClaimState::ACTIVE);
        assert(uart.unit==cellularUartPort(profile->resources.firstGenericUartPort+profile->resources.genericUartCount));
        assert(std::strcmp(uart.ownerId,"runtime:cellular")==0);
        for (uint8_t index=0; index<3; ++index) {
            PeripheralConfig candidate; candidate.id="gpio"; candidate.enabled=true;
            candidate.type=PeripheralType::GPIO_DIGITAL_OUTPUT; candidate.pinCount=1; candidate.pins[0]=pins[index];
            preflightPeripheralChange(*profile,nullptr,0,candidate,PreflightOperation::ADD,"",context,result);
            assert(!result.allowed && result.hasIssue(PreflightIssueCode::GPIO_CONFLICT));
            candidate.enabled=false;
            preflightPeripheralChange(*profile,nullptr,0,candidate,PreflightOperation::ADD,"",context,result);
            assert(result.allowed && result.hasIssue(PreflightIssueCode::DORMANT_GPIO_CONFLICT));
            preflightPeripheralChange(*profile,nullptr,0,candidate,PreflightOperation::ENABLE,"",context,result);
            assert(!result.allowed);
        }
        PeripheralConfig serial; serial.id="generic"; serial.type=PeripheralType::UART; serial.enabled=true;
        serial.pinCount=2; serial.pins[0]=pins[3]; serial.pins[1]=pins[4];
        preflightPeripheralChange(*profile,nullptr,0,serial,PreflightOperation::ADD,"",context,result);
        if (profile->resources.genericUartCount==1) {
            assert(!result.allowed && result.hasIssue(PreflightIssueCode::UART_PORT_EXHAUSTED));
        } else {
            assert(result.allowed);
            auto second=serial; second.id="second"; second.pins[0]=pins[5]; second.pins[1]=pins[6];
            preflightPeripheralChange(*profile,&serial,1,second,PreflightOperation::ADD,"",context,result);
            assert(!result.allowed && result.hasIssue(PreflightIssueCode::UART_PORT_EXHAUSTED));
        }
        // Network candidate versus already-configured resources, both directions.
        serial.id="MoDbUs-main";
        preflightPeripheralChange(*profile,nullptr,0,serial,PreflightOperation::ADD,"",context,result);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::UART_PORT_EXHAUSTED));
        serial.id="rS485-main";
        preflightPeripheralChange(*profile,nullptr,0,serial,PreflightOperation::ADD,"",context,result);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::UART_PORT_EXHAUSTED));
        auto empty=context; empty.cellular.enabled=false;
        PeripheralResourcePlanner reverse(*profile,empty,result);
        ResourceClaim fixed; fixed.kind=ResourceKind::UART_PORT; fixed.state=ClaimState::ACTIVE;
        fixed.unit=cellularUartPort(profile->resources.firstGenericUartPort+profile->resources.genericUartCount);
        fixed.ownerId="existing-protocol"; reverse.addClaim(fixed);
        reverse.addCellular(context.cellular);
        assert(!result.allowed && result.hasIssue(PreflightIssueCode::UART_PORT_EXHAUSTED));
        for (int mode=0; mode<5; ++mode) {
            auto invalid=context.cellular;
            if(mode==0) invalid.rx=-1;
            if(mode==1) invalid.tx=-1;
            if(mode==2) invalid.power=-2;
            if(mode==3) invalid.tx=invalid.rx;
            if(mode==4) invalid.power=invalid.rx;
            PeripheralResourcePlanner bad(*profile,empty,result); bad.addCellular(invalid);
            assert(!result.allowed);
        }
        auto optional=context.cellular; optional.power=-1;
        PeripheralResourcePlanner valid(*profile,empty,result); valid.addCellular(optional);
        assert(result.allowed && result.claimCount==3);
        PeripheralResourcePlanner disabled(*profile,empty,result); optional.enabled=false; optional.rx=-1;
        disabled.addCellular(optional); assert(result.allowed && result.claimCount==0);
    }
}
