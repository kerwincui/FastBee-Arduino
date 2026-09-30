"""Source-level contracts for the bounded smart-panel backend."""

from __future__ import annotations

import pathlib
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(os.environ.get("FASTBEE_TEST_PANEL_SOURCE", pathlib.Path(__file__).resolve().parents[2]))
HEADER = (ROOT / "include/core/ScreenPanelManager.h").read_text(encoding="utf-8")
MANAGER = (ROOT / "src/core/ScreenPanelManager.cpp").read_text(encoding="utf-8")
HISTORY_HEADER = (ROOT / "include/core/PointHistory.h").read_text(encoding="utf-8")
HISTORY = (ROOT / "src/core/PointHistory.cpp").read_text(encoding="utf-8")
ROUTE = (
    ROOT / "src/network/handlers/ScreenPanelRouteHandler.cpp"
).read_text(encoding="utf-8")
ROUTE_HEADER = (
    ROOT / "include/network/handlers/ScreenPanelRouteHandler.h"
).read_text(encoding="utf-8")
FEATURE_FLAGS = (ROOT / "include/core/FeatureFlags.h").read_text(encoding="utf-8")
WEB_HEADER = (ROOT / "include/network/WebConfigManager.h").read_text(
    encoding="utf-8"
)
WEB_MANAGER = (ROOT / "src/network/WebConfigManager.cpp").read_text(
    encoding="utf-8"
)
PERIPH_TYPES = (ROOT / "include/core/PeripheralExecution.h").read_text(
    encoding="utf-8"
)
PERIPH_MANAGER = (ROOT / "src/core/PeriphExecManager.cpp").read_text(
    encoding="utf-8"
)


def braced_block(source: str, marker: str) -> str:
    marker_at = source.find(marker)
    if marker_at < 0:
        raise AssertionError(f"missing source marker {marker!r}")
    opening = source.find("{", marker_at + len(marker))
    if opening < 0:
        raise AssertionError(f"missing block after {marker!r}")
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : index]
    raise AssertionError(f"unterminated block after {marker!r}")


class ScreenPanelContractTests(unittest.TestCase):
    def test_production_dispatch_validates_numeric_inputs_before_run_once(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        signatures = [
            "bool canonicalPanelNumber(const String& text, double& value,\n                          uint8_t* fractionalDigits = nullptr)",
            "bool panelNumberAligned(double value, double origin, double step)",
            "bool fixedModbusAddressTemplate(const String& value)",
            "bool numericActionConstraints(const ExecAction& action,\n                              JsonArrayConst remoteControls,\n                              PanelNumericConstraints& constraints)",
            "PanelRuleInputKind panelRuleInputKind(const PeriphExecRule& rule,\n                                     JsonArrayConst remoteControls,\n                                     PanelNumericConstraints* numeric = nullptr)",
            "bool validatePanelControlInput(PanelRuleInputKind inputKind, const String& visual,\n                               String& error)",
            "bool panelSafeRule(const PeriphExecRule& rule)",
            "String ScreenPanelManager::ruleFingerprint(const PeriphExecRule& rule)",
            "bool ScreenPanelManager::dispatchAction(const char* cardId, ScreenActionKind kind,\n                                        bool target, uint32_t& operationId,\n                                        String& error)",
            "bool ScreenPanelManager::dispatchAction(const char* cardId, ScreenActionKind kind,\n                                        const String& value, uint32_t& operationId,\n                                        String& error, const char* expectedControlRef)",
        ]
        bodies = [sig + " {" + braced_block(MANAGER, sig) + "}" for sig in signatures]
        code = r'''
 #include <cassert>
 #include <array>
 #include <algorithm>
 #include <cctype>
 #include <cmath>
 #include <limits>
#include <Arduino.h>
#include <ArduinoJson.h>
#include "core/PeripheralExecution.h"
#include "core/AsyncExecTypes.h"
inline void convertFromJson(JsonVariantConst source, String& destination) { destination=source.as<std::string>(); }
 enum class PanelRuleInputKind : uint8_t { TRIGGER, BOOLEAN, NUMBER, UNSUPPORTED };
 struct PanelNumericConstraints {
  double minimum=-std::numeric_limits<double>::max();
  double maximum=std::numeric_limits<double>::max();
  bool integerOnly=false,bounded=false;
 };
 enum class ScreenActionKind { SET_BOOLEAN, SET_NUMBER, TRIGGER, SYSTEM_RESTART, SYSTEM_ACTION, MOTOR_COMMAND };
struct MotorControlService {
 static MotorControlService& instance(){static MotorControlService service;return service;}
 unsigned calls=0; String receivedRef;
 bool submit(const String& ref,const String&,int,const String&,uint32_t&,String&){++calls;receivedRef=ref;return true;}
};
enum class RestartReason { USER_COMMAND };
struct SystemRebooter {
 static bool isScheduled(){return false;}
 static void scheduleReboot(const char*,unsigned long,RestartReason){assert(false);}
};
struct PanelLock { explicit PanelLock(void*){} bool locked()const{return true;} };
bool validIdentifier(const String& id) { return !id.isEmpty(); }
struct PeriphExecManager {
 PeriphExecRule rule; int calls=0; String received;
 static PeriphExecManager& getInstance(){static PeriphExecManager value;return value;}
 bool getRuleSnapshot(const String&,PeriphExecRule& out){out=rule;return true;}
 bool runOnce(const String&,const String& value,String*,AsyncExecResult* result){
  ++calls;received=value;result->operationId=42;return true;
 }
};
JsonDocument remoteChoices;
 INPUT_FUNCTIONS
 PanelRuleInputKind resolvePanelRuleInputKind(const PeriphExecRule& rule,PanelNumericConstraints* numeric=nullptr) {
  return panelRuleInputKind(rule,remoteChoices.as<JsonArrayConst>(),numeric);
}
struct ScreenPanelManager {
 JsonDocument config; void* _mutex=nullptr;
 bool _restartSaveFailed=false,_restartWithoutHistorySave=false;
 std::array<String,24> _operationCardIds{}; std::array<uint32_t,24> _operationIds{};
 bool copyConfig(JsonDocument& out,uint32_t& revision,bool& persisted)const {
  out.set(config.as<JsonVariantConst>());revision=1;persisted=true;return true;
 }
  static String ruleFingerprint(const PeriphExecRule&);
  bool dispatchAction(const char*,ScreenActionKind,bool,uint32_t&,String&);
  bool dispatchAction(const char*,ScreenActionKind,const String&,uint32_t&,String&,const char* expectedControlRef=nullptr);
 };
METHODS
int main(){
 auto& exec=PeriphExecManager::getInstance();
 exec.rule.id="write";exec.rule.enabled=true;exec.rule.allowPanelControl=true;
  ExecAction action;action.actionType=17;action.actionValue="5:$value";
  action.useReceivedValue=true;exec.rule.actions.push_back(action);
 ScreenPanelManager panel;
 auto page=panel.config["pages"].add<JsonObject>();
 auto card=page["cards"].add<JsonObject>();
  card["id"]="button";card["kind"]="control";card["visualType"]="number";card["controlRef"]="rule.write";
  card["precision"]=0;card["rangeMin"]=0;card["rangeMax"]=1000;card["step"]=5;card["initialValue"]=0;
  card["bindingFingerprint"]=ScreenPanelManager::ruleFingerprint(exec.rule).c_str();
  uint32_t operation=999;String error;
  assert(panel.dispatchAction("button",ScreenActionKind::SET_NUMBER,String("125"),operation,error));
  assert(exec.calls==1 && exec.received=="125" && operation==42);
  assert(!panel.dispatchAction("button",ScreenActionKind::SET_NUMBER,String("126"),operation,error));
  assert(!panel.dispatchAction("button",ScreenActionKind::SET_NUMBER,String("5:125"),operation,error));
  assert(exec.calls==1 && operation==0);
  exec.rule.actions[0].actionValue="5:100";
  card["bindingFingerprint"]=ScreenPanelManager::ruleFingerprint(exec.rule).c_str();
  assert(!panel.dispatchAction("button",ScreenActionKind::SET_NUMBER,String("125"),operation,error));
  assert(exec.calls==1 && error.indexOf("fixed-value")>=0);
  card["visualType"]="action";
  assert(!panel.dispatchAction("button",ScreenActionKind::TRIGGER,true,operation,error) && exec.calls==1);
  exec.rule.actions[0].useReceivedValue=false;
  card["bindingFingerprint"]=ScreenPanelManager::ruleFingerprint(exec.rule).c_str();
  assert(panel.dispatchAction("button",ScreenActionKind::TRIGGER,true,operation,error));
  assert(exec.calls==2 && exec.received.isEmpty() && operation==42);
  card["visualType"]="switch";
  assert(!panel.dispatchAction("button",ScreenActionKind::SET_BOOLEAN,true,operation,error) && exec.calls==2);
  exec.rule.actions[0].useReceivedValue=true;
  exec.rule.actions[0].actionType=static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL);
  exec.rule.actions[0].actionValue="";
 exec.rule.actions[0].targetPeriphId="lamp";
 auto remote=remoteChoices.to<JsonArray>().add<JsonObject>();
 remote["controlRef"]="remote.lamp";remote["type"]="switch";
 card["bindingFingerprint"]=ScreenPanelManager::ruleFingerprint(exec.rule).c_str();
  assert(panel.dispatchAction("button",ScreenActionKind::SET_BOOLEAN,true,operation,error) && exec.received=="on");
  assert(panel.dispatchAction("button",ScreenActionKind::SET_BOOLEAN,false,operation,error) && exec.received=="off");
  assert(exec.calls==4);
 remote["type"]="action"; // Channel semantics changed without changing the rule fingerprint.
  assert(!panel.dispatchAction("button",ScreenActionKind::SET_BOOLEAN,true,operation,error) && exec.calls==4);
  remote["type"]="switch";exec.rule.allowPanelControl=false;
  assert(!panel.dispatchAction("button",ScreenActionKind::SET_BOOLEAN,true,operation,error) && exec.calls==4);
  exec.rule.allowPanelControl=true;exec.rule.actions[0].actionValue="changed";
  assert(!panel.dispatchAction("button",ScreenActionKind::SET_BOOLEAN,true,operation,error) && exec.calls==4);
  card["kind"]="control";card["visualType"]="motor";card["controlRef"]="motor.new";
  auto& motors=MotorControlService::instance();
  assert(!panel.dispatchAction("button",ScreenActionKind::MOTOR_COMMAND,String("forward"),operation,error,"motor.old"));
  assert(motors.calls==0 && error.indexOf("binding changed")>=0);
  assert(!panel.dispatchAction("button",ScreenActionKind::MOTOR_COMMAND,String("reverse"),operation,error,""));
  assert(motors.calls==0);
  assert(panel.dispatchAction("button",ScreenActionKind::MOTOR_COMMAND,String("stop"),operation,error,"motor.new"));
  assert(motors.calls==1 && motors.receivedRef=="motor.new");
  assert(panel.dispatchAction("button",ScreenActionKind::MOTOR_COMMAND,String("forward"),operation,error,"motor.new"));
  assert(motors.calls==2);
  // Existing callers without a UI expectation retain their previous API contract.
  assert(panel.dispatchAction("button",ScreenActionKind::MOTOR_COMMAND,String("readStatus"),operation,error));
  assert(motors.calls==3);
  // Fixed, explicitly panel-enabled rules remain generic. Relay coil/register
  // and GPIO actions must not pass through the motor command path.
  card["visualType"]="action";card["controlRef"]="rule.write";
  exec.rule.enabled=true;exec.rule.allowPanelControl=true;
  for (auto type : {ExecActionType::ACTION_MODBUS_COIL_WRITE,
                    ExecActionType::ACTION_MODBUS_REG_WRITE,
                    ExecActionType::ACTION_HIGH, ExecActionType::ACTION_LOW,
                    ExecActionType::ACTION_SET_PWM}) {
    exec.rule.actions[0].actionType=static_cast<uint8_t>(type);
    exec.rule.actions[0].useReceivedValue=false;
    exec.rule.actions[0].targetPeriphId="configured-output";
    exec.rule.actions[0].actionValue="0:1";
    card["bindingFingerprint"]=ScreenPanelManager::ruleFingerprint(exec.rule).c_str();
    const int previous=exec.calls;
    assert(panel.dispatchAction("button",ScreenActionKind::TRIGGER,String(""),operation,error));
    assert(exec.calls==previous+1 && operation==42 && motors.calls==3);
    exec.rule.allowPanelControl=false;
    assert(!panel.dispatchAction("button",ScreenActionKind::TRIGGER,String(""),operation,error));
    exec.rule.allowPanelControl=true;exec.rule.actions[0].targetPeriphId="changed-output";
    assert(!panel.dispatchAction("button",ScreenActionKind::TRIGGER,String(""),operation,error));
    assert(exec.calls==previous+1 && motors.calls==3);
  }
 }
 '''.replace("INPUT_FUNCTIONS", "\n".join(bodies[:7])).replace("METHODS", "\n".join(bodies[7:]))
        with tempfile.TemporaryDirectory(prefix="fastbee-panel-input-") as directory:
            temporary = pathlib.Path(directory)
            cpp = temporary / "dispatch.cpp"
            executable = temporary / ("dispatch.exe" if os.name == "nt" else "dispatch")
            cpp.write_text(code, encoding="utf-8")
            env = dict(os.environ)
            env["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + env.get("PATH", "")
            build = subprocess.run([compiler, "-std=c++17", "-DUNIT_TEST",
                                    "-DARDUINOJSON_ENABLE_STD_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=0",
                                    f"-I{ROOT / 'test/mocks'}", f"-I{ROOT / 'include'}",
                                    f"-I{next((ROOT / '.pio/libdeps').glob('*/ArduinoJson/src/ArduinoJson.h')).parent}",
                                    str(cpp), "-o", str(executable)],
                                   capture_output=True, text=True, timeout=60, env=env)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10, env=env)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_history_routes_are_independent_and_screen_ui_requires_panel(self):
        shared_guard = (
            "#if FASTBEE_ENABLE_SCREEN_PANEL || FASTBEE_ENABLE_POINT_HISTORY"
        )
        self.assertIn(shared_guard, ROUTE_HEADER)
        self.assertIn(shared_guard, ROUTE)
        self.assertGreaterEqual(WEB_HEADER.count(shared_guard), 2)
        self.assertGreaterEqual(WEB_MANAGER.count(shared_guard), 3)

        setup = braced_block(
            ROUTE, "void ScreenPanelRouteHandler::setupRoutes(AsyncWebServer* server)"
        )
        panel_guard = setup.index("#if FASTBEE_ENABLE_SCREEN_PANEL")
        panel_route = setup.index('"/api/screen-panel/config"', panel_guard)
        history_guard = setup.index("#if FASTBEE_ENABLE_POINT_HISTORY", panel_route)
        history_route = setup.index('"/api/periph-exec/history"', history_guard)
        self.assertLess(panel_guard, panel_route)
        self.assertLess(history_guard, history_route)
        self.assertIn(
            "#if FASTBEE_ENABLE_SCREEN_UI && !FASTBEE_ENABLE_SCREEN_PANEL",
            FEATURE_FLAGS,
        )
        self.assertIn(
            '#error "FASTBEE_ENABLE_SCREEN_UI requires FASTBEE_ENABLE_SCREEN_PANEL"',
            FEATURE_FLAGS,
        )

        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        source = '#include "core/FeatureFlags.h"\nint main() { return 0; }\n'
        with tempfile.TemporaryDirectory(prefix="fastbee-feature-flags-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "ArduinoJson.h").write_text(
                "class JsonDocument {};\n", encoding="utf-8"
            )
            common = [
                compiler,
                "-std=c++17",
                "-fsyntax-only",
                "-x",
                "c++",
                "-",
                f"-I{temporary}",
                f"-I{ROOT / 'include'}",
                "-DFASTBEE_ENABLE_SCREEN_PANEL=0",
                "-DFASTBEE_ENABLE_POINT_HISTORY=1",
            ]
            valid = subprocess.run(
                common + ["-DFASTBEE_ENABLE_SCREEN_UI=0"],
                input=source,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
            self.assertEqual(valid.returncode, 0, valid.stdout + valid.stderr)
            invalid = subprocess.run(
                common + ["-DFASTBEE_ENABLE_SCREEN_UI=1"],
                input=source,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
            self.assertNotEqual(invalid.returncode, 0)
            self.assertIn(
                "FASTBEE_ENABLE_SCREEN_UI requires FASTBEE_ENABLE_SCREEN_PANEL",
                invalid.stdout + invalid.stderr,
            )

    def test_async_snapshot_uses_local_helpers_and_no_arduino_string(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        if not compiler:
            self.skipTest("A C++17 compiler is required")
        types = (ROOT / "include/core/AsyncExecTypes.h").read_text(encoding="utf-8")
        definitions = []
        for name in ("AsyncExecStatus", "AsyncExecError"):
            marker = f"enum class {name} : uint8_t"
            definitions.append(marker + " {" + braced_block(types, marker) + "};")
        for marker in (
            "const char* panelAsyncExecErrorName(AsyncExecError error)",
            "bool panelAsyncExecStatusIsTerminal(AsyncExecStatus status)",
        ):
            definitions.append(marker + " {" + braced_block(MANAGER, marker) + "}")
        loop_marker = "for (const auto& operation : recentOperations)"
        loop = loop_marker + " {" + braced_block(MANAGER, loop_marker) + "}"
        # Compile the production helpers and snapshot loop without Arduino String
        # or route-handler symbols, so either dependency becomes a build failure.
        with tempfile.TemporaryDirectory(prefix="fastbee-panel-async-") as directory:
            temporary = pathlib.Path(directory)
            (temporary / "panel_async_definitions.inc").write_text(
                "\n".join(definitions), encoding="utf-8"
            )
            (temporary / "panel_async_loop.inc").write_text(loop, encoding="utf-8")
            executable = temporary / ("panel_async.exe" if os.name == "nt" else "panel_async")
            result = subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 f"-I{temporary}",
                 str(ROOT / "test/unit/cpp/screen_panel_async_harness.cpp"),
                 "-o", str(executable)],
                capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=60,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run(
                [str(executable)], capture_output=True, text=True, timeout=15
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_renderer_snapshot_is_fixed_capacity_and_not_stack_duplicated(self):
        for token in (
            "MAX_PAGES = 8",
            "MAX_CARDS = 24",
            "MAX_HISTORY = 10",
            "std::array<ScreenPanelBarValue, 4>",
            "uint8_t brightnessPercent",
            "uint32_t idleSleepSeconds",
        ):
            self.assertIn(token, HEADER)
        self.assertNotIn("ScreenPanelSnapshot snapshot;", MANAGER)
        self.assertIn("new (std::nothrow) ScreenPanelSnapshot()", MANAGER)
        self.assertIn("memset(&out, 0, sizeof(out))", MANAGER)

    def test_config_save_serializes_revision_check_write_and_publish(self):
        save = braced_block(
            MANAGER, "ScreenPanelManager::SaveResult ScreenPanelManager::saveConfig("
        )
        lock_at = save.index("PanelLock lock(_mutex)")
        revision_at = save.index("expectedRevision != _revision", lock_at)
        write_at = save.index("writeEnvelope(", revision_at)
        publish_at = save.index("_configJson = serialized", write_at)
        self.assertLess(lock_at, revision_at)
        self.assertLess(revision_at, write_at)
        self.assertLess(write_at, publish_at)
        self.assertEqual(save.count("PanelLock lock(_mutex)"), 1)

    def test_panel_routes_enforce_read_auth_and_write_developer_mode(self):
        setup = braced_block(
            ROUTE, "void ScreenPanelRouteHandler::setupRoutes(AsyncWebServer* server)"
        )
        for path in (
            '"/api/screen-panel/config"',
            '"/api/screen-panel/choices"',
            '"/api/screen-panel/state"',
            '"/api/periph-exec/history"',
            '"/api/periph-exec/history/config"',
        ):
            self.assertIn(path, setup)

        for marker in (
            "void ScreenPanelRouteHandler::handleGetConfig(",
            "void ScreenPanelRouteHandler::handleChoices(",
            "void ScreenPanelRouteHandler::handleState(",
            "void ScreenPanelRouteHandler::handleHistory(",
            "void ScreenPanelRouteHandler::handleGetHistoryConfig(",
        ):
            self.assertIn("requireAuth(request)", braced_block(ROUTE, marker))
        for marker in (
            "void ScreenPanelRouteHandler::handleSaveConfig(",
            "void ScreenPanelRouteHandler::handleSaveHistoryConfig(",
        ):
            block = braced_block(ROUTE, marker)
            self.assertIn("requireAuth(request)", block)
            self.assertIn("requireDeveloperMode(request)", block)
        self.assertIn("currentRevision", ROUTE)
        self.assertIn("sendJsonResponse(request, 409", ROUTE)

    def test_point_history_is_bounded_deduplicated_and_deadline_flushed(self):
        for token in (
            "MAX_SOURCES = 24",
            "MAX_SAMPLES_PER_SOURCE = 10",
            "FLUSH_DEADLINE_MS = 30000",
            "MAX_FILE_BYTES = 64 * 1024",
        ):
            self.assertIn(token, HISTORY_HEADER)
        record = braced_block(HISTORY, "bool PointHistory::recordSample(")
        self.assertIn("!std::isfinite(value)", record)
        self.assertIn("sample.sampleIdentity == sampleIdentity", record)
        self.assertIn("series->head = (series->head + 1)", record)
        self.assertNotIn("sample.value == value", record)
        tick = braced_block(HISTORY, "void PointHistory::tick(uint32_t nowMs)")
        self.assertIn("FLUSH_DEADLINE_MS", tick)
        self.assertIn('out["points"]', HISTORY)
        flush = braced_block(HISTORY, "bool PointHistory::flushNow(uint32_t timeoutMs)")
        self.assertIn("if (!_initialized) return true", flush)
        pause = braced_block(HISTORY, "bool PointHistory::pauseWrites(uint32_t timeoutMs)")
        self.assertIn("_writesPaused = false", pause)

    def test_restart_touch_path_is_nonblocking_and_force_is_fail_closed(self):
        dispatch = braced_block(
            MANAGER,
            "bool ScreenPanelManager::dispatchAction(const char* cardId, ScreenActionKind kind,\n"
            "                                        const String& value, uint32_t& operationId,",
        )
        self.assertNotIn("flushNow", dispatch)
        self.assertIn("target && !_restartSaveFailed", dispatch)
        self.assertIn('"Panel restart without history"', dispatch)
        self.assertIn("notifyRestartSaveFailure", MANAGER)
        self.assertIn("consumeRestartWithoutHistorySave", MANAGER)
        self.assertIn('String("history_save_failed")', MANAGER)

    def test_peripheral_rules_persist_panel_and_history_permissions(self):
        self.assertIn("bool allowPanelControl = false", PERIPH_TYPES)
        self.assertIn("bool recordHistory = false", PERIPH_TYPES)
        validation_start = PERIPH_MANAGER.index(
            "bool PeriphExecManager::validateRuleActionsLocked("
        )
        validation_end = PERIPH_MANAGER.index(
            "bool PeriphExecManager::addRuleLocked(", validation_start
        )
        validation = PERIPH_MANAGER[validation_start:validation_end]
        self.assertIn("isPanelSafeAction(action.actionType)", validation)
        self.assertIn("rule.recordHistory && !hasRecordableAction", validation)
        self.assertIn("ACTION_REMOTE_READ", validation)


if __name__ == "__main__":
    unittest.main()
