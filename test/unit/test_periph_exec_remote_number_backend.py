"""Retirement coverage for legacy remote number, switch and action commands."""

from __future__ import annotations

import os
import re
import pathlib
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
MANAGER = "\n".join((ROOT / path).read_text(encoding="utf-8") for path in (
    "src/core/PeriphExecManager.cpp", "src/core/PeriphExecRuleQueries.cpp", "include/core/PeriphExecRemoteBindings.h"))
ROUTE = (
    ROOT / "src/network/handlers/PeriphExecRouteHandler.cpp"
).read_text(encoding="utf-8")
REMOTE = (
    ROOT / "src/protocols/RemoteResourceManager.cpp"
).read_text(encoding="utf-8")


def braced_function(source: str, marker: str) -> str:
    marker_at = source.find(marker)
    if marker_at < 0:
        raise AssertionError(f"missing source marker {marker!r}")
    opening = source.find("{", marker_at + len(marker))
    if opening < 0:
        raise AssertionError(f"missing function body after {marker!r}")
    masked = re.sub(r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    lambda m: ' ' * len(m.group()), source)
    depth = 0
    for index in range(opening, len(masked)):
        if masked[index] == "{": depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0: return source[marker_at:index + 1]
    raise AssertionError(f"unterminated function after {marker!r}")


def source_between(source: str, start: str, end: str) -> str:
    start_at = source.find(start)
    if start_at < 0:
        raise AssertionError(f"missing source marker {start!r}")
    end_at = source.find(end, start_at + len(start))
    if end_at < 0:
        raise AssertionError(f"missing end marker {end!r}")
    return source[start_at:end_at]


class RemoteNumberRuleBackendTests(unittest.TestCase):
    def test_event_id_remote_bindings_pause_whole_rules_before_local_actions(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists(): compiler = str(fallback)
        if not compiler: self.skipTest("A C++17 compiler is required")
        helper = braced_function(MANAGER, "bool hasRetiredRemoteSource(")
        sanitize = braced_function(MANAGER, "bool PeriphExecManager::sanitizeRuleForSafety(")
        # Compile the real sanitizer and binding predicate. Other sanitizer
        # dependencies are irrelevant to remote retirement and are stubbed.
        harness = r'''
#include <Arduino.h>
#include <cassert>
#include <vector>
enum class ExecActionType : uint8_t { ACTION_REMOTE_CHANNEL=9 };
struct ExecTrigger { String triggerPeriphId, eventId; };
struct Action { uint8_t actionType=0; };
struct PeriphExecRule {
 bool enabled=true, _cachedHasPollCollectionAction=false, _cachedNeedsModbus=false;
 String name;
 std::vector<ExecTrigger> triggers;
 std::vector<Action> actions;
};
bool ruleHasPollCollectionAction(const PeriphExecRule&) { return false; }
bool ruleNeedsModbus(const PeriphExecRule&) { return false; }
class PeriphExecManager {
public:
 bool sanitizeRuleForSafety(PeriphExecRule&) const;
 bool sanitizeTriggerForSafety(ExecTrigger&,bool,const String&) const { return false; }
};
PRODUCTION
int main() {
 PeriphExecManager manager;
 for (const char* binding : {"remote.lamp","remote.sensor"}) {
   for (bool eventForm : {false,true}) {
     PeriphExecRule rule;
     rule.actions.push_back({0}); // Local relay action must not survive alone.
     rule.triggers.push_back({"button","button_press"});
     rule.triggers.push_back(eventForm ? ExecTrigger{"",String("ds:")+binding} : ExecTrigger{binding,""});
     assert(manager.sanitizeRuleForSafety(rule));
     assert(!rule.enabled && rule.actions.size()==1 && rule.triggers.size()==2);
     assert(!manager.sanitizeRuleForSafety(rule)); // Idempotent migration.
   }
 }
 for (const char* event : {"button_press","ds:weather.local.temperature","ds:local.temperature"}) {
   PeriphExecRule rule; rule.actions.push_back({0}); rule.triggers.push_back({"",event});
   assert(!manager.sanitizeRuleForSafety(rule) && rule.enabled);
 }
}
'''.replace("PRODUCTION", helper + "\n" + sanitize)
        with tempfile.TemporaryDirectory(prefix="fastbee-retired-trigger-") as directory:
            path=pathlib.Path(directory); cpp=path/"retired.cpp"; binary=path/"retired.exe"
            cpp.write_text(harness,encoding="utf-8")
            env=dict(os.environ); env["PATH"]=str(pathlib.Path(compiler).parent)+os.pathsep+env.get("PATH","")
            result=subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",f"-I{ROOT/'test/mocks'}",str(cpp),"-o",str(binary)],capture_output=True,text=True,timeout=60,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=15,env=env)
            self.assertEqual(0,result.returncode,result.stdout+result.stderr)
        for marker in ("bool PeriphExecManager::validateRuleActionsLocked(",
                       "bool PeriphExecManager::setRuleEnabledLocked(",
                       "bool PeriphExecManager::loadConfiguration()",
                       "bool PeriphExecManager::dispatchAsync("):
            self.assertIn("hasRetiredRemoteSource(trigger)", braced_function(MANAGER,marker))
        dispatch=braced_function(MANAGER,"bool PeriphExecManager::dispatchAsync(")
        self.assertLess(dispatch.index("hasRetiredRemoteSource(trigger)"),dispatch.index("shouldAvoidSyncFallback(rule)"))

    def test_retired_actions_are_rejected_before_normal_admission(self):
        validation = source_between(MANAGER,
            "bool PeriphExecManager::validateRuleActionsLocked(",
            "bool PeriphExecManager::addRuleLocked(")
        self.assertIn('REMOTE_CHANNEL_RETIRED: replace the retired remote channel action', validation)
        self.assertIn('REMOTE_CHANNEL_RETIRED: replace the retired remote source', validation)
        self.assertNotIn('validateRemoteChannelRuleAction(', validation)
        preflight = source_between(ROUTE,
            "void PeriphExecRouteHandler::handlePreflightRuleJson(",
            "void PeriphExecRouteHandler::handleAddRuleJson(")
        self.assertIn('validateRemoteChannelRuleAction(', preflight)
        self.assertNotIn('addAction(static_cast<uint8_t>(ExecActionType::ACTION_REMOTE_CHANNEL)', MANAGER)

    def test_production_remote_execution_rejects_numeric_and_boolean_commands(self):
        compiler = shutil.which("g++") or shutil.which("clang++")
        fallback = pathlib.Path("D:/msys64/ucrt64/bin/g++.exe")
        if not compiler and fallback.exists(): compiler = str(fallback)
        if not compiler: self.skipTest("A C++17 compiler is required")
        execute_bool = braced_function(REMOTE, "bool RemoteResourceManager::execute(const String& id,bool target,")
        execute_number = braced_function(REMOTE, "bool RemoteResourceManager::execute(const String& id,const String& numericValue,")
        consume = braced_function(REMOTE, "bool RemoteResourceManager::consumeMqtt(")
        harness = r'''
#include <Arduino.h>
#include <cassert>
class RemoteResourceManager {
public:
 bool execute(const String&,bool,String&,String&);
 bool execute(const String&,const String&,String&,String&);
 bool consumeMqtt(const String&,const String&);
};
PRODUCTION
int main() {
 RemoteResourceManager manager;
 for (const char* id : {"temperature","remote.temperature","lamp","scene",""}) {
   for (const char* value : {"19.25","-12.5","0","$value","on","trigger","NaN",""}) {
     String status="pending",error;
     assert(!manager.execute(String(id),String(value),status,error));
     assert(status=="retired" && error=="Remote channels are retired");
   }
   for (bool target : {true,false}) {
     String status,error;
     assert(!manager.execute(String(id),target,status,error));
     assert(status=="retired" && error=="Remote channels are retired");
   }
 }
 // Dedicated retired topics must not swallow ordinary MQTT protocol handlers.
 assert(!manager.consumeMqtt("home/lamp/state","ON"));
 assert(!manager.consumeMqtt("home/temperature/state","20"));
}
'''.replace("PRODUCTION", execute_bool + execute_number + consume)
        with tempfile.TemporaryDirectory(prefix="fastbee-retired-remote-") as directory:
            temporary = pathlib.Path(directory)
            source = temporary / "retired.cpp"
            executable = temporary / ("retired.exe" if os.name == "nt" else "retired")
            source.write_text(harness, encoding="utf-8")
            environment = dict(os.environ)
            environment["PATH"] = str(pathlib.Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
            compiled = subprocess.run([compiler,"-std=c++17","-Wall","-Wextra","-Werror",
                f"-I{ROOT / 'test/mocks'}",str(source),"-o",str(executable)],
                capture_output=True,text=True,encoding="utf-8",errors="replace",timeout=60,env=environment)
            self.assertEqual(compiled.returncode,0,compiled.stdout+compiled.stderr)
            executed = subprocess.run([str(executable)],capture_output=True,text=True,timeout=15,env=environment)
            self.assertEqual(executed.returncode,0,executed.stdout+executed.stderr)


if __name__ == "__main__":
    unittest.main()
