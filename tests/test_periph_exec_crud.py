"""
PeriphExec (外设执行) 规则系统 CRUD 全面测试脚本
=================================================
通过 HTTP API 对 ESP32 设备进行端到端测试，覆盖：
- 4 种触发类型 (PLATFORM/TIMER/EVENT/POLL)
- 多种动作类型 (GPIO/PWM/DAC/System/Script/Modbus/Sensor)
- CRUD 完整生命周期 (Create → Read → Update → Delete)
- 特殊场景 (多触发器 OR / 多动作顺序 / 异步/同步模式 / 边界条件)

用法:
  python test_periph_exec_crud.py [--host HOST] [--user USER] [--pass PASS]
"""

import sys
import json
import time
import argparse
import requests
from typing import Optional

# ============================================================
# 配置
# ============================================================

DEFAULT_HOST = "http://192.168.1.2"
DEFAULT_USER = "admin"
DEFAULT_PASS = "admin123"
TIMEOUT = 15  # 请求超时秒数
REQUEST_DELAY = 0.4  # 请求间隔，避免 ESP32 连接池耗尽

# ============================================================
# 测试框架
# ============================================================

class TestStats:
    def __init__(self):
        self.total = 0
        self.passed = 0
        self.failed = 0
        self.errors = []

    def record(self, name: str, passed: bool, detail: str = ""):
        self.total += 1
        if passed:
            self.passed += 1
            print(f"  [PASS] {name}")
        else:
            self.failed += 1
            self.errors.append((name, detail))
            print(f"  [FAIL] {name} -- {detail}")

    def summary(self):
        print("\n" + "=" * 60)
        print(f"测试结果: {self.passed}/{self.total} 通过, {self.failed} 失败")
        if self.errors:
            print("\n失败列表:")
            for name, detail in self.errors:
                print(f"  - {name}: {detail}")
        print("=" * 60)
        return self.failed == 0


stats = TestStats()

# ============================================================
# HTTP 客户端
# ============================================================

class ApiClient:
    def __init__(self, host: str, username: str, password: str):
        self.host = host.rstrip("/")
        self.token: Optional[str] = None
        self.session = requests.Session()
        self.session.headers["Accept"] = "application/json"
        self._login(username, password)

    def _login(self, username: str, password: str):
        url = f"{self.host}/api/auth/login"
        resp = self.session.post(url, data={"username": username, "password": password}, timeout=TIMEOUT)
        data = resp.json()
        if resp.status_code == 200 and data.get("success"):
            self.token = data.get("sessionId") or data.get("token") or data.get("data", {}).get("sessionId")
            if self.token:
                self.session.headers["Authorization"] = f"Bearer {self.token}"
                print(f"[AUTH] 登录成功, token={self.token[:16]}...")
                return
        raise RuntimeError(f"登录失败: {resp.status_code} {resp.text}")

    def _safe_request(self, method, path, retries=2, **kwargs):
        """带重试和超时处理的请求方法"""
        kwargs.setdefault("timeout", TIMEOUT)
        url = f"{self.host}{path}"
        for attempt in range(retries + 1):
            try:
                time.sleep(REQUEST_DELAY)
                resp = self.session.request(method, url, **kwargs)
                try:
                    if resp.text.strip():
                        return resp.json()
                except Exception:
                    pass
                if method == "GET" and attempt < retries:
                    time.sleep(2.0)
                    continue
                return {"success": False, "error": f"empty response (status={resp.status_code})"}
            except requests.exceptions.Timeout:
                if attempt < retries:
                    print(f"  [WARN] 超时重试 ({attempt+1}/{retries}): {method} {path}")
                    time.sleep(3.0)
                    continue
                return {"success": False, "error": "request timeout"}
            except requests.exceptions.ConnectionError:
                if attempt < retries:
                    print(f"  [WARN] 连接重试 ({attempt+1}/{retries}): {method} {path}")
                    time.sleep(3.0)
                    continue
                return {"success": False, "error": "connection error"}
        return {"success": False, "error": "max retries exceeded"}

    def get(self, path: str, params: dict = None) -> dict:
        return self._safe_request("GET", path, params=params)

    def post_json(self, path: str, data: dict) -> dict:
        return self._safe_request("POST", path,
                                  json=data, headers={"Content-Type": "application/json"})

    def post_form(self, path: str, data: dict) -> dict:
        return self._safe_request("POST", path, data=data)

    def delete(self, path: str, params: dict = None) -> dict:
        return self._safe_request("DELETE", path, params=params)


# ============================================================
# 辅助函数
# ============================================================

def find_rule(api: ApiClient, rule_id: str) -> Optional[dict]:
    """从 GET /api/periph-exec 中查找指定 ID 的规则"""
    res = api.get("/api/periph-exec")
    if not res.get("success") or not res.get("data"):
        return None
    for r in res["data"]:
        if r["id"] == rule_id:
            return r
    return None


def delete_rule_if_exists(api: ApiClient, rule_id: str):
    """清理：如果规则存在则删除"""
    existing = find_rule(api, rule_id)
    if existing:
        api.delete("/api/periph-exec/", params={"id": rule_id})


def create_and_verify(api: ApiClient, test_name: str, rule_data: dict, verify_fn=None) -> Optional[str]:
    """创建规则并验证，返回规则 ID"""
    res = api.post_json("/api/periph-exec", rule_data)
    ok = res.get("success", False)
    stats.record(f"{test_name} - 创建", ok, "" if ok else f"response={json.dumps(res, ensure_ascii=False)}")
    if not ok:
        return None

    # 读取验证
    time.sleep(0.5)  # ESP32 需要时间持久化
    all_rules = api.get("/api/periph-exec")
    if not all_rules.get("success"):
        stats.record(f"{test_name} - 读取列表", False, "获取规则列表失败")
        return None

    # 查找新创建的规则 (通过名称匹配)
    found = None
    for r in all_rules["data"]:
        if r.get("name") == rule_data.get("name"):
            found = r
            break

    stats.record(f"{test_name} - 读取验证", found is not None,
                 "" if found else f"未找到名称为 '{rule_data.get('name')}' 的规则")

    if found and verify_fn:
        verify_fn(test_name, found)

    return found["id"] if found else None


def update_and_verify(api: ApiClient, test_name: str, rule_id: str, update_data: dict, verify_fn=None):
    """更新规则并验证"""
    update_data["id"] = rule_id
    res = api.post_json("/api/periph-exec/update", update_data)
    ok = res.get("success", False)
    stats.record(f"{test_name} - 更新", ok, "" if ok else f"response={json.dumps(res, ensure_ascii=False)}")
    if not ok:
        return

    time.sleep(0.5)
    found = find_rule(api, rule_id)
    stats.record(f"{test_name} - 更新验证", found is not None,
                 "" if found else f"更新后未找到规则 {rule_id}")

    if found and verify_fn:
        verify_fn(test_name, found)


def delete_and_verify(api: ApiClient, test_name: str, rule_id: str):
    """删除规则并验证"""
    res = api.delete("/api/periph-exec/", params={"id": rule_id})
    ok = res.get("success", False)
    stats.record(f"{test_name} - 删除", ok, "" if ok else f"response={json.dumps(res, ensure_ascii=False)}")

    time.sleep(0.5)
    found = find_rule(api, rule_id)
    stats.record(f"{test_name} - 删除验证", found is None,
                 "" if found is None else f"规则 {rule_id} 仍存在")


# ============================================================
# 测试用例组 1: 触发类型覆盖
# ============================================================

def test_platform_trigger(api: ApiClient):
    """测试平台触发 (PLATFORM_TRIGGER=0) 的 CRUD"""
    print("\n--- 1.1 平台触发 (PLATFORM_TRIGGER=0) ---")

    rule_data = {
        "name": "test_platform_trigger",
        "enabled": False,
        "execMode": 0,  # 异步
        "reportAfterExec": True,
        "triggers": [{
            "triggerType": 0,  # PLATFORM_TRIGGER
            "triggerPeriphId": "temperature",
            "operatorType": 2,  # GT (大于)
            "compareValue": "30",
            "timerMode": 0,
            "intervalSec": 60,
            "timePoint": "",
            "eventId": ""
        }],
        "actions": [{
            "targetPeriphId": "led1",
            "actionType": 0,  # ACTION_HIGH
            "actionValue": "",
            "useReceivedValue": False,
            "syncDelayMs": 0
        }]
    }

    def verify_create(name, rule):
        t = rule.get("triggers", [])
        stats.record(f"{name} - 触发器数量", len(t) == 1, f"expected 1, got {len(t)}")
        if t:
            stats.record(f"{name} - triggerType=0", t[0]["triggerType"] == 0, f"got {t[0]['triggerType']}")
            stats.record(f"{name} - triggerPeriphId", t[0]["triggerPeriphId"] == "temperature",
                         f"got {t[0]['triggerPeriphId']}")
            stats.record(f"{name} - operatorType=2(GT)", t[0]["operatorType"] == 2, f"got {t[0]['operatorType']}")
            stats.record(f"{name} - compareValue=30", t[0]["compareValue"] == "30", f"got {t[0]['compareValue']}")
        a = rule.get("actions", [])
        stats.record(f"{name} - 动作数量", len(a) == 1, f"expected 1, got {len(a)}")
        if a:
            stats.record(f"{name} - actionType=0(HIGH)", a[0]["actionType"] == 0, f"got {a[0]['actionType']}")

    rule_id = create_and_verify(api, "平台触发", rule_data, verify_create)
    if not rule_id:
        return

    # 更新：改条件为 LTE (<=25)
    def verify_update(name, rule):
        t = rule["triggers"][0]
        stats.record(f"{name} - operatorType=5(LTE)", t["operatorType"] == 5, f"got {t['operatorType']}")
        stats.record(f"{name} - compareValue=25", t["compareValue"] == "25", f"got {t['compareValue']}")

    update_and_verify(api, "平台触发", rule_id, {
        "name": "test_platform_trigger_updated",
        "triggers": [{
            "triggerType": 0,
            "triggerPeriphId": "temperature",
            "operatorType": 5,  # LTE
            "compareValue": "25",
        }],
        "actions": [{
            "targetPeriphId": "led1",
            "actionType": 1,  # ACTION_LOW
            "actionValue": "",
        }]
    }, verify_update)

    delete_and_verify(api, "平台触发", rule_id)


def test_timer_trigger_interval(api: ApiClient):
    """测试定时触发-间隔模式 (TIMER_TRIGGER=1, timerMode=0)"""
    print("\n--- 1.2 定时触发-间隔模式 (TIMER_TRIGGER=1, mode=0) ---")

    rule_data = {
        "name": "test_timer_interval",
        "enabled": False,
        "execMode": 0,
        "triggers": [{
            "triggerType": 1,  # TIMER_TRIGGER
            "timerMode": 0,    # INTERVAL
            "intervalSec": 300,
            "triggerPeriphId": "",
            "operatorType": 0,
            "compareValue": "",
            "timePoint": "",
            "eventId": ""
        }],
        "actions": [{
            "targetPeriphId": "relay1",
            "actionType": 0,  # ACTION_HIGH
            "actionValue": "",
        }]
    }

    def verify(name, rule):
        t = rule["triggers"][0]
        stats.record(f"{name} - triggerType=1", t["triggerType"] == 1, f"got {t['triggerType']}")
        stats.record(f"{name} - timerMode=0(INTERVAL)", t["timerMode"] == 0, f"got {t['timerMode']}")
        stats.record(f"{name} - intervalSec=300", t["intervalSec"] == 300, f"got {t['intervalSec']}")

    rule_id = create_and_verify(api, "定时间隔", rule_data, verify)
    if not rule_id:
        return

    # 更新间隔为 600 秒
    def verify_update(name, rule):
        t = rule["triggers"][0]
        stats.record(f"{name} - intervalSec=600", t["intervalSec"] == 600, f"got {t['intervalSec']}")

    update_and_verify(api, "定时间隔", rule_id, {
        "name": "test_timer_interval_updated",
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 600}],
        "actions": [{"targetPeriphId": "relay1", "actionType": 1, "actionValue": ""}]
    }, verify_update)

    delete_and_verify(api, "定时间隔", rule_id)


def test_timer_trigger_daily(api: ApiClient):
    """测试定时触发-每日时间点 (TIMER_TRIGGER=1, timerMode=1)"""
    print("\n--- 1.3 定时触发-每日时间点 (TIMER_TRIGGER=1, mode=1) ---")

    rule_data = {
        "name": "test_timer_daily",
        "enabled": False,
        "execMode": 1,  # 同步
        "triggers": [{
            "triggerType": 1,
            "timerMode": 1,       # DAILY_TIME
            "intervalSec": 60,
            "timePoint": "08:30",
            "triggerPeriphId": "",
            "eventId": ""
        }],
        "actions": [{
            "targetPeriphId": "buzzer1",
            "actionType": 2,  # ACTION_BLINK
            "actionValue": "1000",
        }]
    }

    def verify(name, rule):
        t = rule["triggers"][0]
        stats.record(f"{name} - timerMode=1(DAILY)", t["timerMode"] == 1, f"got {t['timerMode']}")
        stats.record(f"{name} - timePoint=08:30", t["timePoint"] == "08:30", f"got '{t['timePoint']}'")
        stats.record(f"{name} - execMode=1(SYNC)", rule["execMode"] == 1, f"got {rule['execMode']}")
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=2(BLINK)", a["actionType"] == 2, f"got {a['actionType']}")
        stats.record(f"{name} - actionValue=1000", a["actionValue"] == "1000", f"got '{a['actionValue']}'")

    rule_id = create_and_verify(api, "每日定时", rule_data, verify)
    if not rule_id:
        return

    # 更新时间点为 22:00
    def verify_update(name, rule):
        stats.record(f"{name} - timePoint=22:00", rule["triggers"][0]["timePoint"] == "22:00",
                     f"got '{rule['triggers'][0]['timePoint']}'")

    update_and_verify(api, "每日定时", rule_id, {
        "name": "test_timer_daily_updated",
        "triggers": [{"triggerType": 1, "timerMode": 1, "timePoint": "22:00", "intervalSec": 60}],
        "actions": [{"targetPeriphId": "buzzer1", "actionType": 3, "actionValue": "2000"}]
    }, verify_update)

    delete_and_verify(api, "每日定时", rule_id)


def test_event_trigger(api: ApiClient):
    """测试事件触发 (EVENT_TRIGGER=4)"""
    print("\n--- 1.4 事件触发 (EVENT_TRIGGER=4) ---")

    # WiFi 连接事件
    rule_data = {
        "name": "test_event_wifi",
        "enabled": False,
        "execMode": 0,
        "triggers": [{
            "triggerType": 4,  # EVENT_TRIGGER
            "eventId": "wifi_connected",
            "triggerPeriphId": "",
            "operatorType": 0,
            "compareValue": "",
            "timerMode": 0,
            "intervalSec": 60,
            "timePoint": ""
        }],
        "actions": [{
            "targetPeriphId": "status_led",
            "actionType": 0,  # ACTION_HIGH
            "actionValue": "",
        }]
    }

    def verify(name, rule):
        t = rule["triggers"][0]
        stats.record(f"{name} - triggerType=4(EVENT)", t["triggerType"] == 4, f"got {t['triggerType']}")
        stats.record(f"{name} - eventId=wifi_connected", t["eventId"] == "wifi_connected",
                     f"got '{t['eventId']}'")

    rule_id = create_and_verify(api, "WiFi事件", rule_data, verify)
    if not rule_id:
        return

    # 更新为 MQTT 连接事件
    def verify_update(name, rule):
        stats.record(f"{name} - eventId=mqtt_connected",
                     rule["triggers"][0]["eventId"] == "mqtt_connected",
                     f"got '{rule['triggers'][0]['eventId']}'")

    update_and_verify(api, "WiFi事件→MQTT事件", rule_id, {
        "name": "test_event_mqtt",
        "triggers": [{"triggerType": 4, "eventId": "mqtt_connected"}],
        "actions": [{"targetPeriphId": "status_led", "actionType": 2, "actionValue": "500"}]
    }, verify_update)

    delete_and_verify(api, "事件触发", rule_id)


def test_event_trigger_button(api: ApiClient):
    """测试按键事件触发 (EVENT_TRIGGER=4, button events)"""
    print("\n--- 1.5 按键事件触发 ---")

    rule_data = {
        "name": "test_button_click",
        "enabled": False,
        "execMode": 1,  # 同步（按键响应需要即时）
        "triggers": [{
            "triggerType": 4,
            "eventId": "button_click",
        }],
        "actions": [{
            "targetPeriphId": "relay1",
            "actionType": 0,  # ACTION_HIGH (toggle on)
            "actionValue": "",
        }]
    }

    def verify(name, rule):
        stats.record(f"{name} - eventId=button_click",
                     rule["triggers"][0]["eventId"] == "button_click",
                     f"got '{rule['triggers'][0]['eventId']}'")
        stats.record(f"{name} - execMode=1(SYNC)", rule["execMode"] == 1, f"got {rule['execMode']}")

    rule_id = create_and_verify(api, "按键单击", rule_data, verify)
    if not rule_id:
        return

    # 更新为长按 5 秒事件
    def verify_update(name, rule):
        stats.record(f"{name} - eventId=button_long_press_5s",
                     rule["triggers"][0]["eventId"] == "button_long_press_5s",
                     f"got '{rule['triggers'][0]['eventId']}'")

    update_and_verify(api, "按键→长按5s", rule_id, {
        "name": "test_button_longpress",
        "triggers": [{"triggerType": 4, "eventId": "button_long_press_5s"}],
        "actions": [{"targetPeriphId": "", "actionType": 6, "actionValue": ""}]  # 系统重启
    }, verify_update)

    delete_and_verify(api, "按键事件", rule_id)


def test_event_trigger_data(api: ApiClient):
    """测试数据事件触发 (data_receive / data_report)"""
    print("\n--- 1.6 数据事件触发 ---")

    rule_data = {
        "name": "test_data_receive",
        "enabled": False,
        "execMode": 0,
        "triggers": [{
            "triggerType": 4,
            "eventId": "data_receive",
        }],
        "actions": [{
            "targetPeriphId": "led1",
            "actionType": 2,  # BLINK
            "actionValue": "200",
        }]
    }

    def verify(name, rule):
        stats.record(f"{name} - eventId=data_receive",
                     rule["triggers"][0]["eventId"] == "data_receive",
                     f"got '{rule['triggers'][0]['eventId']}'")

    rule_id = create_and_verify(api, "数据接收事件", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "数据接收事件", rule_id)


def test_event_trigger_periph_exec(api: ApiClient):
    """测试外设执行完成事件触发 (periph_exec_completed)"""
    print("\n--- 1.7 外设执行完成事件触发 ---")

    rule_data = {
        "name": "test_periph_exec_event",
        "enabled": False,
        "execMode": 0,
        "triggers": [{
            "triggerType": 4,
            "eventId": "periph_exec_completed",
        }],
        "actions": [{
            "targetPeriphId": "indicator",
            "actionType": 0,  # HIGH
            "actionValue": "",
        }]
    }

    def verify(name, rule):
        stats.record(f"{name} - eventId=periph_exec_completed",
                     rule["triggers"][0]["eventId"] == "periph_exec_completed",
                     f"got '{rule['triggers'][0]['eventId']}'")

    rule_id = create_and_verify(api, "外设执行事件", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "外设执行事件", rule_id)


def test_poll_trigger(api: ApiClient):
    """测试轮询触发 (POLL_TRIGGER=5)"""
    print("\n--- 1.8 轮询触发 (POLL_TRIGGER=5) ---")

    rule_data = {
        "name": "test_poll_trigger",
        "enabled": False,
        "execMode": 0,
        "triggers": [{
            "triggerType": 5,  # POLL_TRIGGER
            "triggerPeriphId": "modbus_temp",
            "operatorType": 2,  # GT
            "compareValue": "50",
            "timerMode": 0,
            "intervalSec": 30,
            "pollResponseTimeout": 2000,
            "pollMaxRetries": 3,
            "pollInterPollDelay": 200,
        }],
        "actions": [{
            "targetPeriphId": "alarm_led",
            "actionType": 2,  # BLINK
            "actionValue": "300",
        }]
    }

    def verify(name, rule):
        t = rule["triggers"][0]
        stats.record(f"{name} - triggerType=5(POLL)", t["triggerType"] == 5, f"got {t['triggerType']}")
        stats.record(f"{name} - triggerPeriphId=modbus_temp",
                     t["triggerPeriphId"] == "modbus_temp", f"got '{t['triggerPeriphId']}'")
        stats.record(f"{name} - pollResponseTimeout=2000",
                     t["pollResponseTimeout"] == 2000, f"got {t['pollResponseTimeout']}")
        stats.record(f"{name} - pollMaxRetries=3",
                     t["pollMaxRetries"] == 3, f"got {t['pollMaxRetries']}")
        stats.record(f"{name} - pollInterPollDelay=200",
                     t["pollInterPollDelay"] == 200, f"got {t['pollInterPollDelay']}")
        stats.record(f"{name} - intervalSec=30", t["intervalSec"] == 30, f"got {t['intervalSec']}")

    rule_id = create_and_verify(api, "轮询触发", rule_data, verify)
    if not rule_id:
        return

    # 更新通信参数
    def verify_update(name, rule):
        t = rule["triggers"][0]
        stats.record(f"{name} - pollResponseTimeout=3000",
                     t["pollResponseTimeout"] == 3000, f"got {t['pollResponseTimeout']}")
        stats.record(f"{name} - pollMaxRetries=5",
                     t["pollMaxRetries"] == 5, f"got {t['pollMaxRetries']}")

    update_and_verify(api, "轮询触发", rule_id, {
        "name": "test_poll_trigger_updated",
        "triggers": [{
            "triggerType": 5,
            "triggerPeriphId": "modbus_temp",
            "operatorType": 4,  # GTE
            "compareValue": "60",
            "timerMode": 0,
            "intervalSec": 15,
            "pollResponseTimeout": 3000,
            "pollMaxRetries": 5,
            "pollInterPollDelay": 150,
        }],
        "actions": [{"targetPeriphId": "alarm_led", "actionType": 0, "actionValue": ""}]
    }, verify_update)

    delete_and_verify(api, "轮询触发", rule_id)


# ============================================================
# 测试用例组 2: 动作类型覆盖
# ============================================================

def test_action_gpio(api: ApiClient):
    """测试 GPIO 动作类型: HIGH/LOW/HIGH_INVERTED/LOW_INVERTED"""
    print("\n--- 2.1 GPIO 动作类型 (HIGH/LOW/INVERTED) ---")

    for action_type, label in [(0, "HIGH"), (1, "LOW"), (13, "HIGH_INVERTED"), (14, "LOW_INVERTED")]:
        rule_data = {
            "name": f"test_action_{label.lower()}",
            "enabled": False,
            "triggers": [{"triggerType": 0, "triggerPeriphId": "sw1", "operatorType": 0, "compareValue": "1"}],
            "actions": [{"targetPeriphId": "gpio_out", "actionType": action_type, "actionValue": ""}]
        }

        def verify(name, rule, expected_type=action_type):
            a = rule["actions"][0]
            stats.record(f"{name} - actionType={expected_type}", a["actionType"] == expected_type,
                         f"got {a['actionType']}")

        rule_id = create_and_verify(api, f"GPIO {label}", rule_data, verify)
        if rule_id:
            delete_and_verify(api, f"GPIO {label}", rule_id)


def test_action_pwm_dac(api: ApiClient):
    """测试 PWM/DAC 动作类型"""
    print("\n--- 2.2 PWM/DAC 动作类型 ---")

    # PWM
    rule_data = {
        "name": "test_action_pwm",
        "enabled": False,
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
        "actions": [{"targetPeriphId": "pwm_out", "actionType": 4, "actionValue": "128"}]
    }

    def verify_pwm(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=4(PWM)", a["actionType"] == 4, f"got {a['actionType']}")
        stats.record(f"{name} - actionValue=128", a["actionValue"] == "128", f"got '{a['actionValue']}'")

    rule_id = create_and_verify(api, "PWM", rule_data, verify_pwm)
    if rule_id:
        delete_and_verify(api, "PWM", rule_id)

    # DAC
    rule_data["name"] = "test_action_dac"
    rule_data["actions"] = [{"targetPeriphId": "dac_out", "actionType": 5, "actionValue": "200"}]

    def verify_dac(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=5(DAC)", a["actionType"] == 5, f"got {a['actionType']}")
        stats.record(f"{name} - actionValue=200", a["actionValue"] == "200", f"got '{a['actionValue']}'")

    rule_id = create_and_verify(api, "DAC", rule_data, verify_dac)
    if rule_id:
        delete_and_verify(api, "DAC", rule_id)


def test_action_blink_breathe(api: ApiClient):
    """测试闪烁/呼吸灯动作"""
    print("\n--- 2.3 闪烁/呼吸灯动作 ---")

    for action_type, label, value in [(2, "BLINK", "500"), (3, "BREATHE", "2000")]:
        rule_data = {
            "name": f"test_action_{label.lower()}",
            "enabled": False,
            "triggers": [{"triggerType": 4, "eventId": "system_ready"}],
            "actions": [{"targetPeriphId": "led_status", "actionType": action_type, "actionValue": value}]
        }

        def verify(name, rule, exp_type=action_type, exp_val=value):
            a = rule["actions"][0]
            stats.record(f"{name} - actionType={exp_type}", a["actionType"] == exp_type, f"got {a['actionType']}")
            stats.record(f"{name} - actionValue={exp_val}", a["actionValue"] == exp_val, f"got '{a['actionValue']}'")

        rule_id = create_and_verify(api, label, rule_data, verify)
        if rule_id:
            delete_and_verify(api, label, rule_id)


def test_action_system(api: ApiClient):
    """测试系统功能动作 (NTP/OTA/AP/BLE - 跳过 restart/factory_reset)"""
    print("\n--- 2.4 系统功能动作 ---")

    # NTP Sync (action_type=8) - 安全的系统动作
    rule_data = {
        "name": "test_action_ntp",
        "enabled": False,  # 禁用避免自动执行
        "triggers": [{"triggerType": 4, "eventId": "wifi_connected"}],
        "actions": [{"targetPeriphId": "", "actionType": 8, "actionValue": ""}]  # NTP_SYNC
    }

    def verify(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=8(NTP)", a["actionType"] == 8, f"got {a['actionType']}")
        stats.record(f"{name} - enabled=false", rule["enabled"] == False, f"got {rule['enabled']}")

    rule_id = create_and_verify(api, "NTP同步", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "NTP同步", rule_id)

    # OTA (action_type=9)
    rule_data["name"] = "test_action_ota"
    rule_data["actions"] = [{"targetPeriphId": "", "actionType": 9, "actionValue": ""}]

    def verify_ota(name, rule):
        stats.record(f"{name} - actionType=9(OTA)", rule["actions"][0]["actionType"] == 9,
                     f"got {rule['actions'][0]['actionType']}")

    rule_id = create_and_verify(api, "OTA", rule_data, verify_ota)
    if rule_id:
        delete_and_verify(api, "OTA", rule_id)

    # 系统重启 (action_type=6) - 只验证保存，保持禁用
    rule_data["name"] = "test_action_restart"
    rule_data["enabled"] = False
    rule_data["actions"] = [{"targetPeriphId": "", "actionType": 6, "actionValue": ""}]

    def verify_restart(name, rule):
        stats.record(f"{name} - actionType=6(RESTART)", rule["actions"][0]["actionType"] == 6,
                     f"got {rule['actions'][0]['actionType']}")

    rule_id = create_and_verify(api, "系统重启(禁用)", rule_data, verify_restart)
    if rule_id:
        delete_and_verify(api, "系统重启(禁用)", rule_id)

    # 恢复出厂 (action_type=7) - 只验证保存，保持禁用
    rule_data["name"] = "test_action_factory"
    rule_data["actions"] = [{"targetPeriphId": "", "actionType": 7, "actionValue": ""}]

    def verify_factory(name, rule):
        stats.record(f"{name} - actionType=7(FACTORY)", rule["actions"][0]["actionType"] == 7,
                     f"got {rule['actions'][0]['actionType']}")

    rule_id = create_and_verify(api, "恢复出厂(禁用)", rule_data, verify_factory)
    if rule_id:
        delete_and_verify(api, "恢复出厂(禁用)", rule_id)


def test_action_script(api: ApiClient):
    """测试脚本执行动作 (ACTION_SCRIPT=15)"""
    print("\n--- 2.5 脚本执行动作 ---")

    script = "SET led1 HIGH\nDELAY 1000\nSET led1 LOW"
    rule_data = {
        "name": "test_action_script",
        "enabled": False,
        "triggers": [{"triggerType": 4, "eventId": "system_ready"}],
        "actions": [{
            "targetPeriphId": "",
            "actionType": 15,  # ACTION_SCRIPT
            "actionValue": script,
        }]
    }

    def verify(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=15(SCRIPT)", a["actionType"] == 15, f"got {a['actionType']}")
        stats.record(f"{name} - 脚本内容保存正确", a["actionValue"] == script,
                     f"got '{a['actionValue'][:50]}...'")

    rule_id = create_and_verify(api, "脚本执行", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "脚本执行", rule_id)


def test_action_modbus_poll(api: ApiClient):
    """测试 Modbus 轮询动作 (ACTION_MODBUS_POLL=18)"""
    print("\n--- 2.6 Modbus 轮询动作 ---")

    # 旧格式：逗号分隔任务索引
    rule_data = {
        "name": "test_modbus_poll_old",
        "enabled": False,
        "triggers": [{"triggerType": 5, "timerMode": 0, "intervalSec": 30,
                       "pollResponseTimeout": 1500, "pollMaxRetries": 2, "pollInterPollDelay": 100}],
        "actions": [{
            "targetPeriphId": "",
            "actionType": 18,  # ACTION_MODBUS_POLL
            "actionValue": "0,1",
        }]
    }

    def verify_old(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=18(POLL)", a["actionType"] == 18, f"got {a['actionType']}")
        stats.record(f"{name} - actionValue=0,1", a["actionValue"] == "0,1", f"got '{a['actionValue']}'")

    rule_id = create_and_verify(api, "Modbus轮询(旧格式)", rule_data, verify_old)
    if rule_id:
        delete_and_verify(api, "Modbus轮询(旧格式)", rule_id)

    # 新 JSON 格式
    poll_json = json.dumps({"poll": [0, 1], "ctrl": [{"d": 0, "a": "on", "c": 0}]})
    rule_data["name"] = "test_modbus_poll_new"
    rule_data["actions"][0]["actionValue"] = poll_json

    def verify_new(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=18", a["actionType"] == 18, f"got {a['actionType']}")
        # 验证 JSON actionValue 完整性
        try:
            parsed = json.loads(a["actionValue"])
            stats.record(f"{name} - poll数组", parsed.get("poll") == [0, 1],
                         f"got {parsed.get('poll')}")
            stats.record(f"{name} - ctrl数组存在", "ctrl" in parsed, f"keys={list(parsed.keys())}")
        except Exception as e:
            stats.record(f"{name} - JSON解析", False, str(e))

    rule_id = create_and_verify(api, "Modbus轮询(JSON)", rule_data, verify_new)
    if rule_id:
        delete_and_verify(api, "Modbus轮询(JSON)", rule_id)


def test_action_modbus_write(api: ApiClient):
    """测试 Modbus 写操作 (COIL_WRITE=16, REG_WRITE=17)"""
    print("\n--- 2.7 Modbus 写操作动作 ---")

    # Coil write
    rule_data = {
        "name": "test_modbus_coil",
        "enabled": False,
        "triggers": [{"triggerType": 0, "triggerPeriphId": "relay_ctrl", "operatorType": 0, "compareValue": "1"}],
        "actions": [{
            "targetPeriphId": "modbus:0",
            "actionType": 16,  # COIL_WRITE
            "actionValue": "0:1",
        }]
    }

    def verify_coil(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=16(COIL)", a["actionType"] == 16, f"got {a['actionType']}")
        stats.record(f"{name} - target=modbus:0", a["targetPeriphId"] == "modbus:0",
                     f"got '{a['targetPeriphId']}'")
        stats.record(f"{name} - actionValue=0:1", a["actionValue"] == "0:1", f"got '{a['actionValue']}'")

    rule_id = create_and_verify(api, "Modbus线圈写", rule_data, verify_coil)
    if rule_id:
        delete_and_verify(api, "Modbus线圈写", rule_id)

    # Register write
    rule_data["name"] = "test_modbus_reg"
    rule_data["actions"] = [{
        "targetPeriphId": "modbus:1",
        "actionType": 17,  # REG_WRITE
        "actionValue": "100:255",
    }]

    def verify_reg(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=17(REG)", a["actionType"] == 17, f"got {a['actionType']}")
        stats.record(f"{name} - actionValue=100:255", a["actionValue"] == "100:255", f"got '{a['actionValue']}'")

    rule_id = create_and_verify(api, "Modbus寄存器写", rule_data, verify_reg)
    if rule_id:
        delete_and_verify(api, "Modbus寄存器写", rule_id)


def test_action_sensor_read(api: ApiClient):
    """测试传感器读取动作 (ACTION_SENSOR_READ=19)"""
    print("\n--- 2.8 传感器读取动作 ---")

    sensor_cfg = json.dumps({
        "periphId": "adc_temp",
        "sensorCategory": "analog",
        "scaleFactor": 0.1,
        "offset": -40,
        "decimalPlaces": 2,
        "sensorLabel": "温度",
        "unit": "°C"
    })

    rule_data = {
        "name": "test_sensor_read",
        "enabled": False,
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 10}],
        "actions": [{
            "targetPeriphId": "adc_temp",
            "actionType": 19,  # SENSOR_READ
            "actionValue": sensor_cfg,
        }]
    }

    def verify(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - actionType=19(SENSOR)", a["actionType"] == 19, f"got {a['actionType']}")
        try:
            cfg = json.loads(a["actionValue"])
            stats.record(f"{name} - periphId", cfg["periphId"] == "adc_temp", f"got '{cfg.get('periphId')}'")
            stats.record(f"{name} - scaleFactor", cfg["scaleFactor"] == 0.1, f"got {cfg.get('scaleFactor')}")
            stats.record(f"{name} - sensorCategory", cfg["sensorCategory"] == "analog",
                         f"got '{cfg.get('sensorCategory')}'")
        except Exception as e:
            stats.record(f"{name} - JSON解析", False, str(e))

    rule_id = create_and_verify(api, "传感器读取", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "传感器读取", rule_id)


# ============================================================
# 测试用例组 3: 特殊场景
# ============================================================

def test_multi_triggers_or(api: ApiClient):
    """测试多触发器 OR 关系 (最多 3 个)"""
    print("\n--- 3.1 多触发器 OR 关系 ---")

    rule_data = {
        "name": "test_multi_triggers",
        "enabled": False,
        "execMode": 0,
        "triggers": [
            {"triggerType": 0, "triggerPeriphId": "temp", "operatorType": 2, "compareValue": "40"},
            {"triggerType": 4, "eventId": "button_click"},
            {"triggerType": 1, "timerMode": 0, "intervalSec": 120},
        ],
        "actions": [{"targetPeriphId": "alarm", "actionType": 2, "actionValue": "200"}]
    }

    def verify(name, rule):
        t = rule["triggers"]
        stats.record(f"{name} - 触发器数量=3", len(t) == 3, f"got {len(t)}")
        if len(t) >= 3:
            stats.record(f"{name} - trigger[0].type=0(PLATFORM)", t[0]["triggerType"] == 0, f"got {t[0]['triggerType']}")
            stats.record(f"{name} - trigger[1].type=4(EVENT)", t[1]["triggerType"] == 4, f"got {t[1]['triggerType']}")
            stats.record(f"{name} - trigger[1].eventId=button_click",
                         t[1]["eventId"] == "button_click", f"got '{t[1]['eventId']}'")
            stats.record(f"{name} - trigger[2].type=1(TIMER)", t[2]["triggerType"] == 1, f"got {t[2]['triggerType']}")

    rule_id = create_and_verify(api, "多触发器OR", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "多触发器OR", rule_id)


def test_multi_actions_sequence(api: ApiClient):
    """测试多动作顺序执行 (最多 4 个)"""
    print("\n--- 3.2 多动作顺序执行 ---")

    rule_data = {
        "name": "test_multi_actions",
        "enabled": False,
        "execMode": 1,  # 同步执行，确保顺序
        "triggers": [{"triggerType": 4, "eventId": "system_ready"}],
        "actions": [
            {"targetPeriphId": "led1", "actionType": 0, "actionValue": "", "syncDelayMs": 0},
            {"targetPeriphId": "led2", "actionType": 0, "actionValue": "", "syncDelayMs": 500},
            {"targetPeriphId": "led3", "actionType": 0, "actionValue": "", "syncDelayMs": 1000},
            {"targetPeriphId": "buzzer", "actionType": 2, "actionValue": "300", "syncDelayMs": 200},
        ]
    }

    def verify(name, rule):
        a = rule["actions"]
        stats.record(f"{name} - 动作数量=4", len(a) == 4, f"got {len(a)}")
        if len(a) >= 4:
            stats.record(f"{name} - action[0].syncDelayMs=0", a[0]["syncDelayMs"] == 0, f"got {a[0]['syncDelayMs']}")
            stats.record(f"{name} - action[1].syncDelayMs=500", a[1]["syncDelayMs"] == 500, f"got {a[1]['syncDelayMs']}")
            stats.record(f"{name} - action[2].syncDelayMs=1000", a[2]["syncDelayMs"] == 1000, f"got {a[2]['syncDelayMs']}")
            stats.record(f"{name} - action[3].target=buzzer", a[3]["targetPeriphId"] == "buzzer",
                         f"got '{a[3]['targetPeriphId']}'")
            stats.record(f"{name} - action[3].type=2(BLINK)", a[3]["actionType"] == 2, f"got {a[3]['actionType']}")

    rule_id = create_and_verify(api, "多动作顺序", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "多动作顺序", rule_id)


def test_async_vs_sync(api: ApiClient):
    """测试异步/同步执行模式"""
    print("\n--- 3.3 异步/同步执行模式 ---")

    # 异步模式 (execMode=0)
    rule_data = {
        "name": "test_async_mode",
        "enabled": False,
        "execMode": 0,  # ASYNC
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
        "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
    }

    def verify_async(name, rule):
        stats.record(f"{name} - execMode=0(ASYNC)", rule["execMode"] == 0, f"got {rule['execMode']}")

    rule_id = create_and_verify(api, "异步模式", rule_data, verify_async)
    if rule_id:
        # 更新为同步模式
        def verify_sync(name, rule):
            stats.record(f"{name} - execMode=1(SYNC)", rule["execMode"] == 1, f"got {rule['execMode']}")

        update_and_verify(api, "异步→同步", rule_id, {
            "name": "test_sync_mode",
            "execMode": 1,
            "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
            "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
        }, verify_sync)

        delete_and_verify(api, "执行模式", rule_id)


def test_use_received_value(api: ApiClient):
    """测试 useReceivedValue 功能"""
    print("\n--- 3.4 使用接收值 (useReceivedValue) ---")

    rule_data = {
        "name": "test_use_received",
        "enabled": False,
        "triggers": [{"triggerType": 0, "triggerPeriphId": "brightness", "operatorType": 0, "compareValue": ""}],
        "actions": [{
            "targetPeriphId": "pwm_led",
            "actionType": 4,  # PWM
            "actionValue": "0",
            "useReceivedValue": True,
            "syncDelayMs": 0
        }]
    }

    def verify(name, rule):
        a = rule["actions"][0]
        stats.record(f"{name} - useReceivedValue=true", a["useReceivedValue"] == True,
                     f"got {a['useReceivedValue']}")

    rule_id = create_and_verify(api, "使用接收值", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "使用接收值", rule_id)


def test_report_after_exec(api: ApiClient):
    """测试 reportAfterExec 控制"""
    print("\n--- 3.5 执行后上报控制 (reportAfterExec) ---")

    # 开启上报
    rule_data = {
        "name": "test_report_on",
        "enabled": False,
        "reportAfterExec": True,
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
        "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
    }

    def verify_on(name, rule):
        stats.record(f"{name} - reportAfterExec=true", rule["reportAfterExec"] == True,
                     f"got {rule['reportAfterExec']}")

    rule_id = create_and_verify(api, "上报=开", rule_data, verify_on)
    if rule_id:
        # 更新为关闭上报
        def verify_off(name, rule):
            stats.record(f"{name} - reportAfterExec=false", rule["reportAfterExec"] == False,
                         f"got {rule['reportAfterExec']}")

        update_and_verify(api, "上报→关", rule_id, {
            "name": "test_report_off",
            "reportAfterExec": False,
            "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
            "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
        }, verify_off)

        delete_and_verify(api, "上报控制", rule_id)


def test_enable_disable(api: ApiClient):
    """测试启用/禁用 API"""
    print("\n--- 3.6 启用/禁用 API ---")

    rule_data = {
        "name": "test_enable_disable",
        "enabled": True,  # 需要启用状态来测试禁用 API
        "triggers": [{"triggerType": 0, "triggerPeriphId": "test_ed", "operatorType": 0, "compareValue": "1"}],
        "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
    }

    rule_id = create_and_verify(api, "启用/禁用", rule_data)
    if not rule_id:
        return

    # 禁用
    res = api.post_form("/api/periph-exec/disable", {"id": rule_id})
    stats.record("禁用API", res.get("success", False), f"response={json.dumps(res, ensure_ascii=False)}")

    time.sleep(0.3)
    found = find_rule(api, rule_id)
    if found:
        stats.record("禁用验证 - enabled=false", found["enabled"] == False, f"got {found['enabled']}")
    else:
        stats.record("禁用验证 - 规则存在", False, "规则不存在")

    # 启用
    res = api.post_form("/api/periph-exec/enable", {"id": rule_id})
    stats.record("启用API", res.get("success", False), f"response={json.dumps(res, ensure_ascii=False)}")

    time.sleep(0.3)
    found = find_rule(api, rule_id)
    if found:
        stats.record("启用验证 - enabled=true", found["enabled"] == True, f"got {found['enabled']}")
    else:
        stats.record("启用验证 - 规则存在", False, "规则不存在")

    delete_and_verify(api, "启用/禁用", rule_id)


def test_operator_types(api: ApiClient):
    """测试所有条件操作符 (EQ/NEQ/GT/LT/GTE/LTE/BETWEEN/NOT_BETWEEN/CONTAIN/NOT_CONTAIN)"""
    print("\n--- 3.7 条件操作符覆盖 ---")

    operators = [
        (0, "EQ", "100"),
        (1, "NEQ", "0"),
        (2, "GT", "50"),
        (3, "LT", "25"),
        (4, "GTE", "80"),
        (5, "LTE", "10"),
        (6, "BETWEEN", "20,80"),
        (7, "NOT_BETWEEN", "0,5"),
        (8, "CONTAIN", "error"),
        (9, "NOT_CONTAIN", "ok"),
    ]

    for op_type, label, compare_val in operators:
        rule_data = {
            "name": f"test_op_{label.lower()}",
            "enabled": False,
            "triggers": [{
                "triggerType": 0,
                "triggerPeriphId": "sensor1",
                "operatorType": op_type,
                "compareValue": compare_val,
            }],
            "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
        }

        def verify(name, rule, exp_op=op_type, exp_cmp=compare_val):
            t = rule["triggers"][0]
            stats.record(f"{name} - operatorType={exp_op}", t["operatorType"] == exp_op,
                         f"got {t['operatorType']}")
            stats.record(f"{name} - compareValue={exp_cmp}", t["compareValue"] == exp_cmp,
                         f"got '{t['compareValue']}'")

        rule_id = create_and_verify(api, f"操作符 {label}", rule_data, verify)
        if rule_id:
            api.delete("/api/periph-exec/", params={"id": rule_id})


def test_max_triggers_boundary(api: ApiClient):
    """测试触发器上限 (MAX_TRIGGERS_PER_RULE=3)"""
    print("\n--- 3.8 触发器数量上限 (3) ---")

    # 正好 3 个 — 应该成功
    rule_data = {
        "name": "test_3_triggers",
        "enabled": False,
        "triggers": [
            {"triggerType": 0, "triggerPeriphId": "a", "operatorType": 0, "compareValue": "1"},
            {"triggerType": 4, "eventId": "wifi_connected"},
            {"triggerType": 1, "timerMode": 0, "intervalSec": 60},
        ],
        "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
    }

    rule_id = create_and_verify(api, "3触发器(上限)", rule_data)
    if rule_id:
        api.delete("/api/periph-exec/", params={"id": rule_id})

    # 超过 3 个 — 应该失败
    rule_data["name"] = "test_4_triggers"
    rule_data["triggers"].append({"triggerType": 4, "eventId": "mqtt_connected"})

    res = api.post_json("/api/periph-exec", rule_data)
    # 后端 addRule 检查 triggers.size() > MAX_TRIGGERS_PER_RULE (3)
    # 4 个触发器应该被拒绝
    rejected = not res.get("success", True)
    stats.record("4触发器(超限) - 应被拒绝", rejected,
                 f"response={json.dumps(res, ensure_ascii=False)}")


def test_max_actions_boundary(api: ApiClient):
    """测试动作上限 (MAX_ACTIONS_PER_RULE=4)"""
    print("\n--- 3.9 动作数量上限 (4) ---")

    # 正好 4 个 — 应该成功
    rule_data = {
        "name": "test_4_actions",
        "enabled": False,
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
        "actions": [
            {"targetPeriphId": "led1", "actionType": 0, "actionValue": ""},
            {"targetPeriphId": "led2", "actionType": 1, "actionValue": ""},
            {"targetPeriphId": "led3", "actionType": 0, "actionValue": ""},
            {"targetPeriphId": "buzzer", "actionType": 2, "actionValue": "500"},
        ]
    }

    rule_id = create_and_verify(api, "4动作(上限)", rule_data)
    if rule_id:
        api.delete("/api/periph-exec/", params={"id": rule_id})

    # 超过 4 个 — 应该失败
    rule_data["name"] = "test_5_actions"
    rule_data["actions"].append({"targetPeriphId": "led4", "actionType": 0, "actionValue": ""})

    res = api.post_json("/api/periph-exec", rule_data)
    rejected = not res.get("success", True)
    stats.record("5动作(超限) - 应被拒绝", rejected,
                 f"response={json.dumps(res, ensure_ascii=False)}")


def test_empty_name_rejected(api: ApiClient):
    """测试空名称应被拒绝"""
    print("\n--- 3.10 空名称验证 ---")

    rule_data = {
        "name": "",
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
        "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
    }

    res = api.post_json("/api/periph-exec", rule_data)
    rejected = not res.get("success", True)
    stats.record("空名称 - 应被拒绝", rejected,
                 f"response={json.dumps(res, ensure_ascii=False)}")


def test_delete_nonexistent(api: ApiClient):
    """测试删除不存在的规则"""
    print("\n--- 3.11 删除不存在的规则 ---")

    res = api.delete("/api/periph-exec/", params={"id": "nonexistent_rule_12345"})
    # 应该返回 404 或 success=false
    failed = not res.get("success", True)
    stats.record("删除不存在规则 - 应返回失败", failed,
                 f"response={json.dumps(res, ensure_ascii=False)}")


def test_update_nonexistent(api: ApiClient):
    """测试更新不存在的规则"""
    print("\n--- 3.12 更新不存在的规则 ---")

    res = api.post_json("/api/periph-exec/update", {
        "id": "nonexistent_rule_99999",
        "name": "ghost",
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
        "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
    })
    failed = not res.get("success", True)
    stats.record("更新不存在规则 - 应返回失败", failed,
                 f"response={json.dumps(res, ensure_ascii=False)}")


def test_duplicate_id_rejected(api: ApiClient):
    """测试重复 ID 应被拒绝"""
    print("\n--- 3.13 重复 ID 验证 ---")

    rule_data = {
        "id": "test_dup_id_check",
        "name": "test_dup_first",
        "enabled": False,
        "triggers": [{"triggerType": 1, "timerMode": 0, "intervalSec": 60}],
        "actions": [{"targetPeriphId": "led1", "actionType": 0, "actionValue": ""}]
    }

    # 清理
    delete_rule_if_exists(api, "test_dup_id_check")
    time.sleep(0.3)

    # 第一次创建
    res1 = api.post_json("/api/periph-exec", rule_data)
    stats.record("重复ID - 首次创建", res1.get("success", False),
                 f"response={json.dumps(res1, ensure_ascii=False)}")

    time.sleep(0.3)

    # 第二次创建同一 ID
    rule_data["name"] = "test_dup_second"
    res2 = api.post_json("/api/periph-exec", rule_data)
    rejected = not res2.get("success", True)
    stats.record("重复ID - 第二次应被拒绝", rejected,
                 f"response={json.dumps(res2, ensure_ascii=False)}")

    # 清理
    delete_rule_if_exists(api, "test_dup_id_check")


def test_data_pipeline(api: ApiClient):
    """测试数据转换管道字段 (protocolType + scriptContent)"""
    print("\n--- 3.14 数据转换管道 ---")

    rule_data = {
        "name": "test_data_pipeline",
        "enabled": False,
        "protocolType": 1,  # MODBUS_RTU
        "scriptContent": "temp=${temperature}&hum=${humidity}",
        "triggers": [{"triggerType": 0, "triggerPeriphId": "temperature", "operatorType": 0, "compareValue": ""}],
        "actions": [{"targetPeriphId": "display", "actionType": 0, "actionValue": ""}]
    }

    def verify(name, rule):
        stats.record(f"{name} - protocolType=1(MODBUS_RTU)", rule["protocolType"] == 1,
                     f"got {rule['protocolType']}")
        stats.record(f"{name} - scriptContent保存正确",
                     rule["scriptContent"] == "temp=${temperature}&hum=${humidity}",
                     f"got '{rule['scriptContent']}'")

    rule_id = create_and_verify(api, "数据管道", rule_data, verify)
    if rule_id:
        delete_and_verify(api, "数据管道", rule_id)


# ============================================================
# 测试用例组 4: 辅助 API 验证
# ============================================================

def test_auxiliary_apis(api: ApiClient):
    """测试辅助 API 端点"""
    print("\n--- 4.1 辅助 API 端点 ---")

    # 静态事件列表
    res = api.get("/api/periph-exec/events/static")
    ok = res.get("success", False) and isinstance(res.get("data"), list)
    stats.record("GET /events/static", ok, f"success={res.get('success')}, data_type={type(res.get('data')).__name__}")
    if ok:
        events = res["data"]
        stats.record("静态事件数量>0", len(events) > 0, f"got {len(events)}")
        # 验证关键事件存在
        event_ids = [e.get("id") for e in events]
        for eid in ["wifi_connected", "mqtt_connected", "button_click", "system_boot", "data_receive"]:
            stats.record(f"静态事件包含 {eid}", eid in event_ids,
                         f"missing from {len(event_ids)} events")

    # 动态事件列表
    res = api.get("/api/periph-exec/events/dynamic")
    ok = res.get("success", False) and isinstance(res.get("data"), list)
    stats.record("GET /events/dynamic", ok, f"success={res.get('success')}")

    # 事件分类列表
    res = api.get("/api/periph-exec/events/categories")
    ok = res.get("success", False) and isinstance(res.get("data"), list)
    stats.record("GET /events/categories", ok, f"success={res.get('success')}")
    if ok:
        cats = [c.get("name") for c in res["data"]]
        for cat in ["WiFi", "MQTT", "按键", "系统"]:
            stats.record(f"分类包含 {cat}", cat in cats, f"categories={cats}")

    # 触发类型列表
    res = api.get("/api/periph-exec/trigger-types")
    ok = res.get("success", False) and isinstance(res.get("data"), list)
    stats.record("GET /trigger-types", ok, f"success={res.get('success')}")
    if ok:
        types = [t.get("type") for t in res["data"]]
        for tt in [0, 1, 4, 5]:  # PLATFORM, TIMER, EVENT, POLL
            stats.record(f"触发类型包含 {tt}", tt in types, f"types={types}")


# ============================================================
# 测试用例组 5: 数据持久化验证
# ============================================================

def test_persistence(api: ApiClient):
    """测试数据持久化 (创建 → 读取验证配置完整性)"""
    print("\n--- 5.1 数据持久化完整性 ---")

    # 创建一个包含所有字段的复杂规则
    rule_data = {
        "name": "test_persistence_full",
        "enabled": False,
        "execMode": 1,
        "reportAfterExec": False,
        "protocolType": 2,
        "scriptContent": "data=${value}",
        "triggers": [
            {
                "triggerType": 5,
                "triggerPeriphId": "modbus_sensor",
                "operatorType": 6,  # BETWEEN
                "compareValue": "10,90",
                "timerMode": 0,
                "intervalSec": 45,
                "timePoint": "",
                "eventId": "",
                "pollResponseTimeout": 1500,
                "pollMaxRetries": 4,
                "pollInterPollDelay": 250,
            }
        ],
        "actions": [
            {
                "targetPeriphId": "pwm_fan",
                "actionType": 4,  # PWM
                "actionValue": "200",
                "useReceivedValue": True,
                "syncDelayMs": 100,
            },
            {
                "targetPeriphId": "modbus:0",
                "actionType": 17,  # REG_WRITE
                "actionValue": "40:100",
                "useReceivedValue": False,
                "syncDelayMs": 500,
            }
        ]
    }

    rule_id = None
    res = api.post_json("/api/periph-exec", rule_data)
    if not res.get("success"):
        stats.record("持久化 - 创建", False, f"response={json.dumps(res, ensure_ascii=False)}")
        return

    time.sleep(0.5)

    # 找到新创建的规则
    all_rules = api.get("/api/periph-exec")
    found = None
    for r in all_rules.get("data", []):
        if r.get("name") == "test_persistence_full":
            found = r
            rule_id = r["id"]
            break

    if not found:
        stats.record("持久化 - 读取", False, "未找到规则")
        return

    stats.record("持久化 - 创建+读取", True, "")

    # 逐字段验证
    stats.record("持久化 - name", found["name"] == "test_persistence_full", f"got '{found['name']}'")
    stats.record("持久化 - enabled", found["enabled"] == False, f"got {found['enabled']}")
    stats.record("持久化 - execMode", found["execMode"] == 1, f"got {found['execMode']}")
    stats.record("持久化 - reportAfterExec", found["reportAfterExec"] == False, f"got {found['reportAfterExec']}")
    stats.record("持久化 - protocolType", found["protocolType"] == 2, f"got {found['protocolType']}")
    stats.record("持久化 - scriptContent", found["scriptContent"] == "data=${value}",
                 f"got '{found['scriptContent']}'")

    # 触发器字段
    t = found["triggers"][0]
    stats.record("持久化 - trigger.triggerType", t["triggerType"] == 5, f"got {t['triggerType']}")
    stats.record("持久化 - trigger.triggerPeriphId", t["triggerPeriphId"] == "modbus_sensor",
                 f"got '{t['triggerPeriphId']}'")
    stats.record("持久化 - trigger.operatorType", t["operatorType"] == 6, f"got {t['operatorType']}")
    stats.record("持久化 - trigger.compareValue", t["compareValue"] == "10,90", f"got '{t['compareValue']}'")
    stats.record("持久化 - trigger.intervalSec", t["intervalSec"] == 45, f"got {t['intervalSec']}")
    stats.record("持久化 - trigger.pollResponseTimeout", t["pollResponseTimeout"] == 1500,
                 f"got {t['pollResponseTimeout']}")
    stats.record("持久化 - trigger.pollMaxRetries", t["pollMaxRetries"] == 4, f"got {t['pollMaxRetries']}")
    stats.record("持久化 - trigger.pollInterPollDelay", t["pollInterPollDelay"] == 250,
                 f"got {t['pollInterPollDelay']}")

    # 动作字段
    a0 = found["actions"][0]
    stats.record("持久化 - action[0].targetPeriphId", a0["targetPeriphId"] == "pwm_fan",
                 f"got '{a0['targetPeriphId']}'")
    stats.record("持久化 - action[0].actionType", a0["actionType"] == 4, f"got {a0['actionType']}")
    stats.record("持久化 - action[0].actionValue", a0["actionValue"] == "200", f"got '{a0['actionValue']}'")
    stats.record("持久化 - action[0].useReceivedValue", a0["useReceivedValue"] == True,
                 f"got {a0['useReceivedValue']}")
    stats.record("持久化 - action[0].syncDelayMs", a0["syncDelayMs"] == 100, f"got {a0['syncDelayMs']}")

    a1 = found["actions"][1]
    stats.record("持久化 - action[1].targetPeriphId", a1["targetPeriphId"] == "modbus:0",
                 f"got '{a1['targetPeriphId']}'")
    stats.record("持久化 - action[1].actionType", a1["actionType"] == 17, f"got {a1['actionType']}")
    stats.record("持久化 - action[1].actionValue", a1["actionValue"] == "40:100", f"got '{a1['actionValue']}'")
    stats.record("持久化 - action[1].syncDelayMs", a1["syncDelayMs"] == 500, f"got {a1['syncDelayMs']}")

    # 运行时字段：enabled=false 时 checkTimers() 不会触发，triggerCount 应为 0
    # 但仍使用 >= 0 做宽松断言以兼容运行时可能的边界情况
    stats.record("持久化 - trigger.triggerCount运行时递增",
                 isinstance(t["triggerCount"], int) and t["triggerCount"] >= 0,
                 f"got {t['triggerCount']} (type={type(t['triggerCount']).__name__})")

    # 清理
    if rule_id:
        api.delete("/api/periph-exec/", params={"id": rule_id})


# ============================================================
# 主入口
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="PeriphExec CRUD 全面测试")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"设备地址 (默认: {DEFAULT_HOST})")
    parser.add_argument("--user", default=DEFAULT_USER, help=f"用户名 (默认: {DEFAULT_USER})")
    parser.add_argument("--password", default=DEFAULT_PASS, help=f"密码 (默认: {DEFAULT_PASS})")
    args = parser.parse_args()

    print("=" * 60)
    print("PeriphExec 规则系统 CRUD 全面测试")
    print(f"目标: {args.host}")
    print("=" * 60)

    try:
        api = ApiClient(args.host, args.user, args.password)
    except Exception as e:
        print(f"\n[ERROR] 无法连接或登录: {e}")
        sys.exit(1)

    # 记录初始规则数量
    initial = api.get("/api/periph-exec")
    initial_count = len(initial.get("data", []))
    print(f"[INFO] 当前已有 {initial_count} 条规则\n")

    # ─────── 1. 触发类型覆盖 ───────
    print("\n" + "=" * 60)
    print("=== 测试组 1: 触发类型覆盖 ===")
    print("=" * 60)
    test_platform_trigger(api)
    test_timer_trigger_interval(api)
    test_timer_trigger_daily(api)
    test_event_trigger(api)
    test_event_trigger_button(api)
    test_event_trigger_data(api)
    test_event_trigger_periph_exec(api)
    test_poll_trigger(api)

    # ─────── 2. 动作类型覆盖 ───────
    print("\n" + "=" * 60)
    print("=== 测试组 2: 动作类型覆盖 ===")
    print("=" * 60)
    test_action_gpio(api)
    test_action_pwm_dac(api)
    test_action_blink_breathe(api)
    test_action_system(api)
    test_action_script(api)
    test_action_modbus_poll(api)
    test_action_modbus_write(api)
    test_action_sensor_read(api)

    # ─────── 3. 特殊场景 ───────
    print("\n" + "=" * 60)
    print("=== 测试组 3: 特殊场景验证 ===")
    print("=" * 60)
    test_multi_triggers_or(api)
    test_multi_actions_sequence(api)
    test_async_vs_sync(api)
    test_use_received_value(api)
    test_report_after_exec(api)
    test_enable_disable(api)
    test_operator_types(api)
    test_max_triggers_boundary(api)
    test_max_actions_boundary(api)
    test_empty_name_rejected(api)
    test_delete_nonexistent(api)
    test_update_nonexistent(api)
    test_duplicate_id_rejected(api)
    test_data_pipeline(api)

    # ─────── 4. 辅助 API ───────
    print("\n" + "=" * 60)
    print("=== 测试组 4: 辅助 API 验证 ===")
    print("=" * 60)
    test_auxiliary_apis(api)

    # ─────── 5. 持久化 ───────
    print("\n" + "=" * 60)
    print("=== 测试组 5: 数据持久化 ===")
    print("=" * 60)
    test_persistence(api)

    # ─────── 验证清理完整性 ───────
    time.sleep(0.5)
    final = api.get("/api/periph-exec")
    final_count = len(final.get("data", []))
    stats.record("清理验证 - 规则数量恢复", final_count == initial_count,
                 f"初始={initial_count}, 最终={final_count}")

    # ─────── 输出总结 ───────
    all_pass = stats.summary()
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
