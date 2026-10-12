#include "core/PeriphExecExecutor.h"
#include "core/UartAction.h"
#include "core/PeripheralManager.h"
#include <cassert>
#include <string>

PeriphExecExecutor::PeriphExecExecutor()=default;
PeriphExecExecutor::~PeriphExecExecutor()=default;

int main(int argc, char** argv) {
    assert(argc==2);
    const std::string mode=argv[1];
    ExecAction action; action.actionType=29; action.targetPeriphId="uart-a";
    action.actionValue="  hello  "; action.useReceivedValue=false;
    String received, expected="  hello  "; const char* error=nullptr;
    auto& manager=PeripheralManager::getInstance();
    if(mode.find("queued_")==0) {
        PeriphExecExecutor executor;ActionExecResult result;bool isUart=true;uint32_t epoch=99;
        action.actionType=10;
        if(mode=="queued_changed" || mode=="queued_retyped")manager.version=8;
        if(mode=="queued_retyped" || mode=="queued_gpio")manager.config.type=PeripheralType::GPIO_DIGITAL_OUTPUT;
        if(mode=="queued_missing")manager.found=false;
        if(mode=="queued_busy")rtcMutexBusy=true;
        const bool resolved=executor.resolveUartActionTarget(action,7,isUart,epoch,&result);
        const bool admitted=mode=="queued_normal" || mode=="queued_gpio" || mode=="queued_changed_after_resolve";
        assert(resolved==admitted && manager.writes==0 && rtcLockDepth==0);
        if(!admitted) {
            assert(!isUart && epoch==0);
            assert(result.remark==(mode=="queued_busy"?"UART_OR_CALL_BUSY":mode=="queued_missing"?
                "UART_OR_CALL_TARGET_INVALID":"UART_OR_CALL_CONFIG_CHANGED"));
        } else {
            assert(epoch==7 && isUart==(mode!="queued_gpio"));
            if(isUart) {
                if(mode=="queued_changed_after_resolve")manager.version=8;
                const bool sent=executor.executeUartSendAction(action,action.actionValue,"",epoch,result);
                assert(sent==(mode=="queued_normal"));assert(manager.writes==(sent?1U:0U));
            }
        }
        return 0;
    }
    if(mode=="generic") action.actionType=10;
    if(mode=="literal_command") {action.actionValue="send";expected="send";}
    if(mode=="json_literal") {action.actionValue=R"({"action":"send","value":"{\"v\":1}"})";expected="{\"v\":1}";}
    if(mode=="json_injection") {
        action.actionValue=R"({"periphId":"uart-a","action":"send","value":"$value"})";
        received="\",\"periphId\":\"other\",\"value\":\"injected\\x"; expected=received;
    }
    if(mode=="target_redirect") {action.actionValue=R"({"periphId":"other","value":"hello"})";error="UART_TARGET_MISMATCH";}
    if(mode=="target_template") {action.actionValue=R"({"periphId":"$value","value":"hello"})";received="uart-a";error="UART_TARGET_MISMATCH";}
    if(mode=="invalid_command") {action.actionValue=R"({"action":"toggle","value":"hello"})";error="UART_COMMAND_INVALID";}
    if(mode=="numeric_payload") {action.actionValue=R"({"value":1})";error="UART_PAYLOAD_TYPE_INVALID";}
    if(mode=="nul_payload") {action.actionValue=R"({"value":"a\u0000b"})";error="UART_PAYLOAD_TYPE_INVALID";}
    if(mode=="two_fields") {action.actionValue=R"({"value":"one","text":"two"})";error="UART_PAYLOAD_FIELD_INVALID";}
    if(mode=="unknown_field") {action.actionValue=R"({"value":"hello","repeat":2})";error="UART_PAYLOAD_FIELD_INVALID";}
    if(mode=="malformed") {action.actionValue="{bad";error="UART_PAYLOAD_JSON_INVALID";}
    if(mode=="empty_json") {action.actionValue=R"({"value":""})";error="UART_PAYLOAD_EMPTY";}
    if(mode=="empty") {action.actionValue="";error="UART_PAYLOAD_EMPTY";}
    if(mode=="unresolved") {action.actionValue="$value";error="UART_VALUE_UNRESOLVED";}
    if(mode=="repeated") {action.actionValue="$value/$value";received="OK";expected="OK/OK";}
    if(mode=="nonrecursive") {action.actionValue="$value";received="x$value";expected=received;}
    if(mode=="direct") {action.actionValue="";action.useReceivedValue=true;received="raw $value";expected=received;}
    if(mode=="max_bytes") {action.actionValue=std::string(256,'x');expected=action.actionValue;}
    if(mode=="oversize") {action.actionValue=std::string(257,'x');error="UART_PAYLOAD_TOO_LARGE";}
    if(mode=="expanded_oversize") {action.actionValue="$value$value";received=std::string(129,'x');error="UART_PAYLOAD_TOO_LARGE";}
    if(mode=="utf8_bytes") {action.actionValue=std::string(254,'x')+"\xe4\xb8\xad";error="UART_PAYLOAD_TOO_LARGE";}
    if(mode=="changed") {manager.version=8;error="UART_CONFIG_CHANGED";}
    if(mode=="missing") {manager.found=false;error="UART_CONFIG_CHANGED";}
    if(mode=="wrong_type") {manager.config.type=PeripheralType::GPIO_DIGITAL_OUTPUT;error="UART_CONFIG_CHANGED";}
    if(mode=="disabled") {manager.config.enabled=false;error="UART_NOT_READY";}
    if(mode=="no_state") {manager.stateFound=false;error="UART_NOT_READY";}
    if(mode=="suspended") {manager.runtime.memorySuspended=true;error="UART_NOT_READY";}
    if(mode=="lost_lease") {manager.runtime.state.comm.isConnected=false;error="UART_NOT_READY";}
    if(mode=="busy") {rtcMutexBusy=true;error="UART_BUSY";}
    if(mode=="partial") {manager.writeOk=false;error="UART_WRITE_UNCONFIRMED";}
    if(mode=="wrong_action") {action.actionType=28;error="UART_ACTION_INVALID";}
    String effective=action.useReceivedValue?received:action.actionValue;
    PeriphExecExecutor executor; ActionExecResult result;
    result.success=true;result.actualValue="OLD";result.remark="OLD";
    const bool ok=executor.executeUartSendAction(action,effective,received,7,result);
    assert(rtcLockDepth==0 && result.actualValue.isEmpty() && result.targetPeriphId=="uart-a");
    if(error) {
        assert(!ok && !result.success && result.remark==error);
        assert(manager.writes==(mode=="partial"?1U:0U));
    } else {
        assert(ok && result.success && manager.writes==1 && manager.sent==expected);
        assert(result.remark=="UART_LOCAL_ACCEPTED_PEER_UNVERIFIED" && rtcLockTimeout==0);
    }
    if(mode=="expanded_oversize") assert(FastBee::validateUartAction(action)==nullptr);
    if(mode=="oversize" || mode=="target_redirect" || mode=="invalid_command")
        assert(FastBee::validateUartAction(action)!=nullptr);
}
