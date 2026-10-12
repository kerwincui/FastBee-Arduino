"""设备端示例与当前固件契约的轻量回归。

该测试读取相邻 FastBee-doc checkout；它不访问设备，也不把 Markdown 中的历史
验收记录当作当前通过证据。目的在于防止示例重新出现已经修复的字段/调度语义。
"""
from __future__ import annotations

import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DOC_ROOT = Path(r"D:/project/gitlab/FastBee-doc/src/device")
EXAMPLES = DOC_ROOT / "examples"
AUDIT = ROOT / "artifacts/device-doc-audit-20261011/examples-review.json"


def _markdown_files() -> list[Path]:
    return sorted(EXAMPLES.glob("*.md"))


def _fenced_json(text: str):
    for match in re.finditer(r"```json\s*\n([\s\S]*?)\n```", text):
        raw = match.group(1).strip()
        try:
            yield json.loads(raw)
        except json.JSONDecodeError:
            # ecm50a 保留的 network 片段是文档中的字段片段，不能误当整份 JSON。
            if '"cellular": {' not in raw and '"mode": 0' not in raw:
                raise AssertionError(f"invalid JSON example: {raw[:100]}")


def test_all_device_examples_are_audited_and_json_is_parseable():
    files = _markdown_files()
    assert len(files) == 64, "examples inventory changed; update the audit deliberately"
    report = json.loads(AUDIT.read_text(encoding="utf-8"))
    reviewed = {item["path"] for item in report["files"]}
    expected = {f"examples/{p.name}" for p in files}
    assert reviewed == expected
    for path in files:
        list(_fenced_json(path.read_text(encoding="utf-8")))


def test_scheduler_and_action_values_used_by_examples_match_firmware():
    scheduler = (ROOT / "include/core/PeriphExecScheduler.h").read_text(encoding="utf-8")
    execution = (ROOT / "include/core/PeripheralExecution.h").read_text(encoding="utf-8")
    assert "MIN_TIMER_INTERVAL_SEC = 1" in scheduler
    assert "MIN_POLL_INTERVAL_SEC = 5" in scheduler
    assert "ACTION_SCRIPT = 15" in execution
    assert "ACTION_OLED_DISPLAY = 27" in execution
    assert "ACTION_SD_RECORD = 40" in execution

    text = "\n".join(p.read_text(encoding="utf-8") for p in _markdown_files())
    assert "intervalSec: 0.5" not in text
    assert "intervalSec: 0.1" not in text
    assert "事件类型`100`" not in text
    assert "getVar('" not in text
    assert "servoWrite('" not in text
    # The supported action examples use the actual data-source and template forms.
    assert "ds:dht_01_humidity" in text
    assert "${dht1.temperature}" in text
    assert "syncDelayMs" in text


def test_screen_panel_limits_and_document_are_consistent():
    header = (ROOT / "include/core/ScreenPanelManager.h").read_text(encoding="utf-8")
    history = (ROOT / "include/core/PointHistory.h").read_text(encoding="utf-8")
    doc = (DOC_ROOT / "system/screen-panel.md").read_text(encoding="utf-8")
    assert "MAX_PAGES = 8" in header
    assert "MAX_CARDS = 24" in header
    assert "MAX_SOURCES = 24" in history
    assert "MAX_SAMPLES_PER_SOURCE = 10" in history
    assert "最多8页、24张卡片" in doc
    assert "最多24个来源、每源10点" in doc
    assert "touchUiMigration=2" in doc
    assert "手动刷新至少间隔5分钟" in doc
