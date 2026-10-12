"""Documentation contracts for current device execution/protocol behavior."""
from pathlib import Path
import json
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
DOC_ROOT = Path(r"D:/project/gitlab/FastBee-doc/src/device")
SRC_ROOT = ROOT / "src"
INCLUDE_ROOT = ROOT / "include"

def read(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig")

class DeviceExecutionDocumentationTests(unittest.TestCase):
    def test_action_ids_and_system_boundaries_match_headers(self):
        header = read(INCLUDE_ROOT / "core/PeripheralExecution.h")
        config = read(DOC_ROOT / "periph-exec/periph-exec-configuration-guide.md")
        for name, number in (("ACTION_NEOPIXEL_EFFECT", 11), ("ACTION_STEPPER_CONTROL", 12), ("ACTION_SCRIPT", 15), ("ACTION_MODBUS_POLL", 18), ("ACTION_THERMOSTAT", 39)):
            self.assertRegex(header, rf"{name}\s*=\s*{number}\b")
            self.assertIn(str(number), config)
        executor = read(SRC_ROOT / "core/PeriphExecExecutor.cpp")
        system = read(DOC_ROOT / "periph-exec/system-actions.md")
        self.assertIn("Factory reset rejected", executor)
        self.assertIn("ACTION_SYS_OTA", executor)
        self.assertIn("动作 7", system)
        self.assertIn("拒绝自动化恢复出厂", system)
        self.assertIn("当前执行返回失败", system)

    def test_remote_resource_docs_follow_retired_routes_and_weather_limits(self):
        model = read(INCLUDE_ROOT / "protocols/RemoteResourceModel.h")
        routes = read(SRC_ROOT / "network/handlers/RemoteResourceRouteHandler.cpp")
        doc = read(DOC_ROOT / "protocols/remote-resources.md")
        self.assertRegex(model, r"MAX_CHANNELS\s*=\s*0")
        self.assertRegex(model, r"MAX_WEATHER\s*=\s*1")
        self.assertIn('"/api/remote-resources"', routes)
        self.assertIn("410", routes)
        self.assertIn("心知天气", doc)
        self.assertIn("旧入口停用", doc)
        self.assertIn("最多一个活动天气来源", doc)

    def test_script_docs_use_production_commands_and_limits(self):
        engine = read(SRC_ROOT / "core/ScriptEngine.cpp")
        actions = read(DOC_ROOT / "periph-exec/script-actions.md")
        guide = read(DOC_ROOT / "periph-exec/script-guide.md")
        self.assertIn("MQTT <topicIndex> <message", actions)
        self.assertIn("1024", actions)
        self.assertIn("50", actions)
        self.assertIn("PERIPH/DELAY/GPIO/PWM/DAC/LOG/MQTT", guide)
        self.assertIn("不提供通用 JavaScript", guide)
        self.assertIn("MAX_SCRIPT_SIZE", engine)
        self.assertIn("MAX_COMMANDS", engine)

    def test_examples_separate_collection_from_data_event_rules(self):
        for name in ("light-control.md", "ultrasonic-alarm.md", "smoke-alarm.md", "temperature-alarm.md"):
            body = read(DOC_ROOT / "periph-exec" / name)
            self.assertIn("ds:", body, name)
            self.assertIn('actionType": 19', body, name)
        poll = read(DOC_ROOT / "periph-exec/poll-trigger.md")
        self.assertIn("不会先用本轮结果检查触发器条件", poll)
        self.assertIn("采集与告警分成两条规则", poll)

    def test_fenced_json_in_changed_execution_docs_is_parseable(self):
        changed = ["periph-exec/periph-exec-configuration-guide.md", "periph-exec/periph-exec-flow.md", "periph-exec/platform-trigger.md", "periph-exec/poll-trigger.md", "periph-exec/timer-trigger.md", "periph-exec/system-actions.md", "periph-exec/script-actions.md", "periph-exec/script-guide.md", "periph-exec/gpio-actions.md", "periph-exec/pwm-actions.md", "periph-exec/display-actions.md", "periph-exec/button-control.md", "periph-exec/event-actions.md", "periph-exec/light-control.md", "periph-exec/ultrasonic-alarm.md", "periph-exec/smoke-alarm.md", "periph-exec/temperature-alarm.md", "periph-exec/modbus-actions.md", "periph-exec/modbus-monitor.md", "protocols/modbus-rtu.md", "protocols/mqtt-config.md", "protocols/remote-resources.md", "system/periph-exec-management.md", "system/rule-script.md"]
        fence = re.compile(r"```json\s*([\s\S]*?)```", re.I)
        for relative in changed:
            body = read(DOC_ROOT / relative)
            for block in fence.findall(body):
                try:
                    json.loads(block)
                except json.JSONDecodeError as exc:
                    self.fail(f"{relative}: invalid fenced JSON: {exc}")

if __name__ == "__main__":
    unittest.main()
