#include <Arduino.h>
#include "core/ScriptEngine.h"
#include "core/DisplayOutputTypes.h"
#include "core/ActionValueContract.h"
#include "core/ReportInstanceBinding.h"
#include "core/FeatureObservationCounter.h"
#include <cassert>
#include <cstring>
#include <functional>

static unsigned long clockMs = 0;
static unsigned writes = 0, tickerStops = 0, publications = 0, displays = 0;
static unsigned leases = 0, acquisitions = 0;
static bool expansionComplete = true;
static std::function<void()> onDelay, onWrite, onLookup, onClaim, onStop, onTemplate, onRandom, onAcquire;
static std::function<void()> onPwmRelease;
static void fire(std::function<void()>& hook) {
    if (hook) { auto callback = std::move(hook); hook = {}; callback(); }
}
unsigned long scriptClock() { return clockMs; }
void scriptWait(unsigned long ms) {
    assert(FastBee::FeatureInstanceRegistry::instance().observe("commandScript").state == FastBee::FeatureInstanceState::Running);
    assert(leases == 0); clockMs += ms; fire(onDelay);
}
#define millis scriptClock
#define delay scriptWait
constexpr int OUTPUT = 1;
void pinMode(uint8_t,int) {}
void digitalWrite(uint8_t,int) {
    assert(FastBee::FeatureInstanceRegistry::instance().observe("commandScript").state == FastBee::FeatureInstanceState::Running);
    assert(leases == 0); ++writes; fire(onWrite);
}
static bool pwmAttachOk=true,pwmWriteOk=true;
bool ledcAttach(uint8_t,int,int) { return pwmAttachOk; }
bool ledcWrite(uint8_t,uint32_t) { ++writes; fire(onWrite); return pwmWriteOk; }
void dacWrite(uint8_t,uint8_t) { ++writes; fire(onWrite); }
enum class GPIOState { STATE_LOW, STATE_HIGH };
enum class PeripheralType { GPIO_DIGITAL_OUTPUT, GPIO_PWM_OUTPUT, GPIO_ANALOG_OUTPUT, DAC, PWM_SERVO, LCD, SEVEN_SEGMENT_TM1637 };
struct PeripheralConfig {
    PeripheralType type = PeripheralType::GPIO_DIGITAL_OUTPUT;
    bool expander = true, modbus = false;
    struct { struct {
        bool passive = false, aoMode = false;
        uint8_t pwmResolution = 8, aoMinPct = 0, aoMaxPct = 100;
    } gpio; } params;
    bool isExpanderChannel() const { return expander; }
    bool isModbusPeripheral() const { return modbus; }
};
struct PeripheralManager {
    bool current = true, busy = false;
    bool namedWriteAccepted = true;
    // Protocol cases exercise the typed source boundary; no legacy ticker stop.
    bool namedNative = false, namedModbus = false;
    PeripheralType namedType = PeripheralType::GPIO_DIGITAL_OUTPUT;
    unsigned ordinaryCalls = 0;
    bool submitOrdinaryDisplay(const String&, FastBee::DisplayOutputCommand, const String&, int32_t,
        const FastBee::OrdinaryControlContext* context, FastBee::OrdinaryControlResult* result) {
        ++ordinaryCalls; submittedContext = context;
        if (!context || !isUartEventOriginCurrent(context->uartOrigin)) return false;
        assert(context->request.operation && context->request.actor.identity);
        ++displays;
        if (result) { result->evaluated = true; result->accepted = true; }
        return true; // Actual shared policy/ACK failures use display_ordinary_harness.
    }
    bool submitOrdinaryLcd(const String&, const FastBee::LcdOutputRequest& output,
        const FastBee::OrdinaryControlContext* context, FastBee::OrdinaryControlResult* result) {
        ++ordinaryCalls; submittedContext=context;
        if(!context || !isUartEventOriginCurrent(context->uartOrigin)) return false;
        assert(context->request.operation && context->request.actor.identity);
        ++displays;
        if(output.command==FastBee::LcdOutputCommand::Clear) fire(onWrite);
        if(result) { result->evaluated=true; result->accepted=true; }
        return true;
    }
    bool submitOrdinaryServo(const String&, int64_t, bool,
        const FastBee::OrdinaryControlContext*, FastBee::OrdinaryControlResult*) {
        assert(false); return false; // Actual servo branch/source tests use the motion fixture.
    }
    bool submitOrdinaryScalar(const String& id, FastBee::ScalarOutputCommand command, uint32_t value,
        const FastBee::OrdinaryControlContext& context, FastBee::OrdinaryControlResult& result) {
        ++ordinaryCalls; submittedContext = &context;
        assert(context.request.operation && context.request.actor.identity);
        assert(command == FastBee::ScalarOutputCommand::Pwm);
        result.accepted = writePWM(id, value); return result.accepted;
    }
    const FastBee::OrdinaryControlContext* submittedContext = nullptr;
    FastBee::DigitalOutputCommand submittedCommand = FastBee::DigitalOutputCommand::Low;
    unsigned namedWriteCalls = 0;
    String namedWriteId;
    GPIOState namedWriteLevel = GPIOState::STATE_LOW;
    uint32_t namedWriteDuty = 0;
    static PeripheralManager& getInstance() { static PeripheralManager instance; return instance; }
    bool isUartEventOriginCurrent(const FastBee::UartEventOrigin& origin) const {
        return origin.unbound() || (current && !busy && origin.matches(7,19));
    }
    int getMutex() { return 1; }
    bool controlOutputPinIsClaimed(uint8_t) { fire(onClaim); return false; }
    bool writeScriptPwm(uint8_t pin,uint32_t duty) { return ledcAttach(pin,5000,8) && ledcWrite(pin,duty); }
    bool releaseScriptPwm(uint8_t) { fire(onPwmRelease); return true; } // Actual ledger has independent fault regressions.
    bool prepareScriptPin(uint8_t pin,uint8_t kind) { return kind == 1 || releaseScriptPwm(pin); }
    bool writeScriptGpio(uint8_t pin,bool high) { digitalWrite(pin,high); return true; }
    bool writeScriptDac(uint8_t pin,uint8_t value) { dacWrite(pin,value); return true; }
    bool hasPeripheral(const String&) { fire(onLookup); return true; }
    bool getPeripheralSnapshot(const String& id, PeripheralConfig& result) {
        result = PeripheralConfig{}; result.type = namedType;
        result.expander = !namedNative; result.modbus = namedModbus;
        if (id == "lcd" || id == "segment") {
            result.type = id == "lcd" ? PeripheralType::LCD : PeripheralType::SEVEN_SEGMENT_TM1637;
            result.expander = false; result.modbus = false;
        }
        return true;
    }
    void stopActionTicker(const String&) { ++tickerStops; fire(onStop); }
    bool startActionTicker(const String&,int,uint16_t,const FastBee::UartEventOrigin& origin) {
        if (!isUartEventOriginCurrent(origin)) return false;
        ++writes; return true;
    }
    bool writeModbusPin(const String& id,GPIOState value,
        const FastBee::OrdinaryControlContext& context,FastBee::OrdinaryControlResult& result) {
        ++ordinaryCalls;submittedContext=&context;result.evaluated=true;
        fire(onStop); // Existing fault point now models retirement at typed admission.
        if(!isUartEventOriginCurrent(context.uartOrigin))return false;
        assert(context.request.operation && context.request.actor.identity);
        result.accepted=writePin(id,value);return result.accepted;
    }
    bool writeModbusPWM(const String& id,uint32_t value,
        const FastBee::OrdinaryControlContext& context,FastBee::OrdinaryControlResult& result) {
        ++ordinaryCalls;submittedContext=&context;result.evaluated=true;
        if(!isUartEventOriginCurrent(context.uartOrigin))return false;
        assert(context.request.operation && context.request.actor.identity);
        result.accepted=writePWM(id,value);return result.accepted;
    }
    bool writePin(const String& id,GPIOState value) {
        ++namedWriteCalls; namedWriteId=id; namedWriteLevel=value;
        if (!namedWriteAccepted) return false;
        ++writes; fire(onWrite); return true;
    }
    bool writePWM(const String& id,uint32_t value) {
        ++namedWriteCalls; namedWriteId=id; namedWriteDuty=value;
        if (!namedWriteAccepted) return false;
        ++writes; fire(onWrite); return true;
    }
    // Admission policy/checked timers have their own production PM harness.
    // This seam only observes the exact context passed through script layers.
    bool submitOrdinaryDigital(const String& id, FastBee::DigitalOutputCommand command,
        uint32_t, const FastBee::OrdinaryControlContext& context,
        FastBee::OrdinaryControlResult& result) {
        ++ordinaryCalls; submittedContext = &context; submittedCommand = command;
        assert(context.request.operation && context.request.actor.identity);
        assert(command == FastBee::DigitalOutputCommand::High || command == FastBee::DigitalOutputCommand::Low);
        result.accepted = writePin(id, command == FastBee::DigitalOutputCommand::High
            ? GPIOState::STATE_HIGH : GPIOState::STATE_LOW);
        return result.accepted;
    }
};
struct RecursiveMutexGuard {
    RecursiveMutexGuard(int,int) {}
    bool isLocked() const { return true; }
};
struct Logger {
    template<class... T> void infof(T...) {}
    template<class... T> void warningf(T...) {}
    void warning(const char*) {}
} LOGGER;
struct MQTTClient {
    uint32_t instance = 11;
    bool connected = true, accepting = true;
    uint32_t getReportInstanceId() const { return instance; }
    bool getIsConnected() const { return connected; }
    bool queueTopicMessage(size_t topic,const String& payload) {
        assert(topic == 0 && payload == "payload");
        ++publications; return accepting;
    }
};
static MQTTClient* activeMqtt = nullptr;
struct TestLease {
    MQTTClient* client;
    explicit TestLease(MQTTClient* p):client(p) { if(client) ++leases; }
    TestLease(const TestLease&) = delete;
    TestLease(TestLease&& other):client(other.client) { other.client=nullptr; }
    ~TestLease() { if(client) --leases; }
    explicit operator bool() const { return client != nullptr; }
    MQTTClient* operator->() const { return client; }
    MQTTClient* get() const { return client; }
};
struct ExecAction { String actionValue = "script"; };
static std::vector<ScriptCommand> parsedCommands;
std::vector<ScriptCommand> ScriptEngine::parse(const String&) { return parsedCommands; }
bool ScriptEngine::validate(const std::vector<ScriptCommand>&,String&) { return true; }
namespace FastBee {
struct RuntimeFeatureManager {
    static RuntimeFeatureManager& instance() { static RuntimeFeatureManager manager; return manager; }
    bool shouldEnable(const char*) const { return true; }
};
}
struct PeriphExecExecutor {
    static String resolveSensorTemplate(const String& value) { fire(onTemplate); return value; }
    TestLease getMqttClient() { ++acquisitions; fire(onAcquire); return TestLease(activeMqtt); }
    uint32_t captureReportInstanceId() {
        return FastBee::captureReportInstance([this] { return getMqttClient(); });
    }
    bool executeScriptAction(const ExecAction&, const FastBee::UartEventOrigin&, bool*,
        const FastBee::OrdinaryControlContext* = nullptr, FastBee::OrdinaryControlResult* = nullptr);
};
bool isLcdPeripheral(const String& id) { return id == "lcd"; }
struct LCDManager {
    static LCDManager& getInstance() { static LCDManager instance; return instance; }
    bool showCustomText(const String&) { ++displays; return true; }
    bool showSensorPage(int8_t) { ++displays; return true; }
    int getSensorEntryCount() { return 0; }
    void clear() { ++displays; fire(onWrite); }
    void refresh() { ++displays; }
};
struct SevenSegmentDriver {
    static SevenSegmentDriver& instance() { static SevenSegmentDriver instance; return instance; }
    void displayNumber(const String&,const String&) { ++displays; }
    void displayText(const String&,const String&) { ++displays; }
    void clear(const String&) { ++displays; }
    void setBrightness(const String&,uint8_t) { ++displays; }
};
bool ScriptEngine::processRandomExpressions(const String& value, String& output) {
    fire(onRandom);
    if (!expansionComplete) { output.clear(); return false; }
    output=value; return true;
}

// @production@

int main(int argc,char** argv) {
    assert(argc == 2); const String scenario = argv[1];
    auto& pm = PeripheralManager::getInstance();
    auto origin = FastBee::UartEventOrigin::captured(7,19);
    ScriptCommand gpio{}; gpio.type=ScriptCmdType::CMD_GPIO; gpio.pin=1; gpio.intParam=1;
    ScriptCommand pause{}; pause.type=ScriptCmdType::CMD_DELAY; pause.intParam=20;
    ScriptCommand peripheral{}; peripheral.type=ScriptCmdType::CMD_PERIPH;
    peripheral.strParam="led"; peripheral.subAction="HIGH";
    ScriptCommand publish{}; publish.type=ScriptCmdType::CMD_MQTT; publish.strParam="payload";
    if (scenario.startsWith("native_")) {
        pm.namedNative = true;
        peripheral.subAction = scenario.startsWith("native_low_") ? "LOW" :
            scenario.startsWith("native_blink_") ? "BLINK" :
            scenario.startsWith("native_stop_") ? "STOP" : "HIGH";
        const bool prefix = scenario.endsWith("_after_prefix");
        parsedCommands = {peripheral, gpio};
        if (prefix) parsedCommands.insert(parsedCommands.begin(), gpio);
        PeriphExecExecutor executor;
        bool sourceRejected = true;
        FastBee::OrdinaryControlResult result;
        assert(!executor.executeScriptAction(ExecAction{}, origin, &sourceRejected, nullptr, &result));
        assert(!sourceRejected && result.evaluated && !result.accepted);
        assert(!strcmp(result.reason, "CONTROL_CONTEXT_REQUIRED") && !strcmp(result.targetId, "led"));
        assert(pm.ordinaryCalls == 0 && pm.namedWriteCalls == 0 && tickerStops == 0);
        assert(writes == (prefix ? 1u : 0u) && publications == 0 && acquisitions == 0 && leases == 0);
        return 0;
    }
    if (scenario.startsWith("periph_")) {
        const bool pwm = scenario.startsWith("periph_pwm_");
        const bool native = scenario.startsWith("periph_native_");
        const bool low = scenario.startsWith("periph_low_") || scenario.startsWith("periph_native_low_");
        peripheral.subAction = pwm ? "PWM" : low ? "LOW" : "HIGH";
        peripheral.intParam = 128;
        const bool accepted = scenario.endsWith("_success");
        const bool prefix = scenario.endsWith("_after_prefix");
        pm.namedNative = native || pwm;
        pm.namedType = pwm ? PeripheralType::GPIO_PWM_OUTPUT : PeripheralType::GPIO_DIGITAL_OUTPUT;
        pm.namedWriteAccepted = accepted;
        parsedCommands = {peripheral, gpio};
        if (prefix) parsedCommands.insert(parsedCommands.begin(), gpio);
        PeriphExecExecutor executor;
        bool sourceRejected = true;
        FastBee::OrdinaryControlContext context;
        context.request.actor = {FastBee::OrdinaryControl::Source::Rule, 7};
        context.request.operation = 23; context.configEpoch = 7; context.uartOrigin = origin;
        FastBee::OrdinaryControlResult result;
        const bool ok = executor.executeScriptAction(ExecAction{}, origin, &sourceRejected, &context, &result);
        assert(ok == accepted && !sourceRejected);
        // A rejected command is tried once. No following output, replay or
        // rollback of an already accepted prefix is allowed by the script.
        assert(pm.namedWriteCalls == 1 && pm.namedWriteId == "led");
        assert(writes == (accepted ? 2u : prefix ? 1u : 0u));
        assert(tickerStops == 0 && pm.ordinaryCalls == 1);
        assert(pm.submittedContext == &context && result.evaluated && result.accepted == accepted);
        if (!pwm) {
            assert(pm.submittedCommand == (low ? FastBee::DigitalOutputCommand::Low : FastBee::DigitalOutputCommand::High));
        }
        if (pwm) assert(pm.namedWriteDuty == 128);
        else assert(pm.namedWriteLevel == (low ? GPIOState::STATE_LOW : GPIOState::STATE_HIGH));
        assert(publications == 0 && acquisitions == 0 && leases == 0);
        return 0;
    }
    if (scenario.startsWith("bind_")) {
        MQTTClient client; activeMqtt=&client;
        parsedCommands={pause,publish,gpio};
        bool expected=true, sourceFailed=false;
        unsigned expectedPublishes=1, expectedOutputs=1;
        if(scenario=="bind_valid") {}
        else if(scenario=="bind_rebuild_delay") {onDelay=[&]{client.instance=12;};expected=false;expectedPublishes=expectedOutputs=0;}
        else if(scenario=="bind_unknown") {client.instance=0;onDelay=[&]{client.instance=11;};expected=false;expectedPublishes=expectedOutputs=0;}
        else if(scenario=="bind_absent_start") {activeMqtt=nullptr;onDelay=[&]{activeMqtt=&client;};expected=false;expectedPublishes=expectedOutputs=0;}
        else if(scenario=="bind_absent_publish") {onDelay=[&]{activeMqtt=nullptr;};expected=false;expectedPublishes=expectedOutputs=0;}
        else if(scenario=="bind_offline") {client.connected=false;} // Local retention permits the next command; not delivery.
        else if(scenario=="bind_rejected") {client.accepting=false;expected=false;expectedOutputs=0;}
        else if(scenario=="bind_failed_after_gpio") {parsedCommands={gpio,publish,gpio};client.accepting=false;expected=false;}
        else if(scenario=="bind_no_mqtt") {parsedCommands={pause,gpio};expectedPublishes=0;}
        else if(scenario=="bind_rebuild_after_first") {parsedCommands={publish,pause,publish,gpio};onDelay=[&]{client.instance=12;};expected=false;expectedOutputs=0;}
        else if(scenario=="bind_source_after_acquire") {
            onDelay=[&]{onAcquire=[&]{pm.current=false;};};
            expected=false;sourceFailed=true;expectedPublishes=expectedOutputs=0;
        }
        else if(scenario=="bind_disabled") {expected=false;expectedPublishes=expectedOutputs=0;}
        else assert(false);
        PeriphExecExecutor executor;bool sourceRejected=false;
        const bool ok=executor.executeScriptAction(ExecAction{},origin,&sourceRejected);
        assert(ok==expected && sourceRejected==sourceFailed);
        assert(publications==expectedPublishes && writes==expectedOutputs && leases==0);
        if(scenario=="bind_no_mqtt" || scenario=="bind_disabled") assert(acquisitions==0);
        return 0;
    }
    std::vector<ScriptCommand> commands{gpio,gpio};
    auto retire=[&](){pm.current=false;};
    unsigned expectedWrites=0, expectedDisplays=0, expectedPublications=0;
    bool expectedOk=false, expectedRejected=true;
    if(scenario=="stale") pm.current=false;
    else if(scenario=="malformed") origin.required=false;
    else if(scenario=="busy") pm.busy=true;
    else if(scenario=="empty_stale") {pm.current=false;commands.clear();}
    else if(scenario=="delay") {commands={pause,gpio};onDelay=retire;}
    else if(scenario=="last_delay") {commands={pause};onDelay=retire;}
    else if(scenario=="between") {onWrite=retire;expectedWrites=1;}
    else if(scenario=="last_write") {commands={gpio};onWrite=retire;expectedWrites=1;}
    else if(scenario=="claim") onClaim=retire;
    else if(scenario=="pwm_release_retired") onPwmRelease=retire;
    else if(scenario=="pwm_attach_failure" || scenario=="pwm_write_failure") {
        ScriptCommand pwm=gpio;pwm.type=ScriptCmdType::CMD_PWM;commands={pwm,gpio};
        pwmAttachOk=scenario!="pwm_attach_failure";pwmWriteOk=false;
        expectedRejected=false;expectedWrites=pwmAttachOk?1:0;
    }
    else if(scenario=="lookup") {commands={peripheral};onLookup=retire;}
    else if(scenario=="stop_then_write") {
        pm.namedModbus=true;commands={peripheral};onStop=retire;
    }
    else if(scenario=="lcd_template" || scenario=="segment_template") {
        peripheral.strParam=scenario=="lcd_template" ? "lcd" : "segment";
        peripheral.subAction="TEXT";peripheral.extraParam="${sensor.temperature}";
        commands={peripheral};onTemplate=retire;
    }
    else if(scenario=="mqtt") {commands={publish};onRandom=retire;}
    else if(scenario=="mqtt_missing" || scenario=="mqtt_negative") {
        if(scenario=="mqtt_negative") publish.intParam=-1;
        commands={publish,gpio};expectedRejected=false;
    }
    else if(scenario=="mqtt_expansion" || scenario=="log_expansion") {
        if(scenario=="log_expansion") publish.type=ScriptCmdType::CMD_LOG;
        commands={publish,gpio};expansionComplete=false;expectedRejected=false;
    }
    else if(scenario=="valid" || scenario=="unbound") {
        ScriptCommand pwm=gpio;pwm.type=ScriptCmdType::CMD_PWM;
        ScriptCommand dac=gpio;dac.type=ScriptCmdType::CMD_DAC;
        commands={gpio,pause,pwm,dac,peripheral,publish};
        expectedOk=true;expectedRejected=false;expectedWrites=4;expectedPublications=1;
        if(scenario=="unbound") {origin={};pm.current=false;}
    }
    else if(scenario=="lcd_clear") {
        peripheral.strParam="lcd";peripheral.subAction="CLEAR";
        commands={peripheral};onWrite=retire;expectedDisplays=1;
    }
    else assert(false);
    bool rejected=true;MQTTClient mqtt;
    ScriptMqttPublisher publisher{&mqtt,[](void* raw,size_t topic,const String& message) {
        return FastBee::publishBoundScriptTopic(static_cast<MQTTClient*>(raw),11,topic,message);
    }};
    if(scenario=="mqtt_missing") publisher={};
    FastBee::OrdinaryControlContext ordinary;
    ordinary.request.actor={FastBee::OrdinaryControl::Source::Rule,7};
    ordinary.request.operation=23; ordinary.configEpoch=7; ordinary.uartOrigin=origin;
    const bool ok=ScriptEngine::execute(commands,publisher,origin,&rejected,&ordinary);
    const auto activity = FastBee::FeatureInstanceRegistry::instance().observe("commandScript");
    assert(activity.scope == FastBee::FeatureInstanceScope::ExecutionActivity &&
        activity.state == FastBee::FeatureInstanceState::Idle && !activity.countKnown());
    assert(ok==expectedOk && rejected==expectedRejected);
    assert(writes==expectedWrites && displays==expectedDisplays && publications==expectedPublications);
    if(scenario=="stop_then_write") assert(tickerStops==0 && pm.ordinaryCalls==1 && !pm.namedWriteCalls);
    if(scenario=="delay" || scenario=="last_delay") assert(clockMs==10);
}
