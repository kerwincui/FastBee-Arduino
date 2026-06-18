/**
 * 网络配置页面 UI 完整性验证脚本
 * 用法: node scripts/validate-network-ui.js
 *
 * 验证内容：
 * 1. network.html 必须包含各联网方式保存按钮的 id
 * 2. network.js 必须包含 _startSaveBtnCountdown 共享辅助函数
 * 3. network.js 必须包含各联网方式的重启提示信息
 */
'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const NETWORK_HTML = path.join(ROOT_DIR, 'web-src', 'pages', 'network.html');
const NETWORK_JS = path.join(ROOT_DIR, 'web-src', 'modules', 'runtime', 'network.js');

let errors = 0;
let checks = 0;

function check(condition, label) {
    checks++;
    if (condition) {
        console.log(`  ✓ ${label}`);
    } else {
        console.error(`  ✗ ${label}`);
        errors++;
    }
}

console.log('\n=== 网络配置页面 UI 完整性验证 ===\n');

// ─── 1. HTML 按钮 ID 验证 ─────────────────────────────────────────────────────
console.log('[1] network.html 按钮 ID 验证');

const htmlContent = fs.readFileSync(NETWORK_HTML, 'utf8');

// 各联网方式的保存按钮必须有 id
const REQUIRED_BUTTON_IDS = [
    { id: 'wifi-save-btn',       label: 'WiFi 保存按钮' },
    { id: 'wifi-save-btn-text',  label: 'WiFi 保存按钮文字 span' },
    { id: 'ethernet-save-btn',   label: '以太网保存按钮' },
    { id: 'ethernet-save-btn-text', label: '以太网保存按钮文字 span' },
    { id: 'cellular-save-btn',   label: '4G 保存按钮' },
    { id: 'cellular-save-btn-text', label: '4G 保存按钮文字 span' },
    { id: 'lora-save-btn',       label: 'LoRa 保存按钮' },
    { id: 'lora-save-btn-text',  label: 'LoRa 保存按钮文字 span' },
];

for (const { id, label } of REQUIRED_BUTTON_IDS) {
    const regex = new RegExp(`id=["']${id}["']`);
    check(regex.test(htmlContent), `${label} (id="${id}")`);
}

// ─── 2. JS 函数存在性验证 ────────────────────────────────────────────────────
console.log('\n[2] network.js 核心函数验证');

const jsContent = fs.readFileSync(NETWORK_JS, 'utf8');

// _startSaveBtnCountdown 共享倒计时辅助函数
check(
    /_startSaveBtnCountdown\s*\(/.test(jsContent),
    '_startSaveBtnCountdown() 共享倒计时函数'
);

// 各保存函数必须存在
const REQUIRED_SAVE_FUNCTIONS = [
    { fn: 'saveNetworkConfig',    label: 'WiFi 保存函数' },
    { fn: 'saveEthernetConfig',   label: '以太网保存函数' },
    { fn: 'saveCellularConfig',   label: '4G 保存函数' },
    { fn: 'saveLoRaConfig',       label: 'LoRa 保存函数' },
    { fn: 'saveActiveNetworkConfig', label: '统一保存入口函数' },
];

for (const { fn, label } of REQUIRED_SAVE_FUNCTIONS) {
    const regex = new RegExp(`\\b${fn}\\s*\\(`);
    check(regex.test(jsContent), `${label} (${fn})`);
}

// ─── 3. 重启提示信息验证 ────────────────────────────────────────────────────
console.log('\n[3] 网络重启提示信息验证');

// 各联网方式必须包含重启提示关键字
const RESTART_HINTS = [
    { text: '网络配置变更需要重启网络服务才能生效', label: '通用重启提示文案' },
    { text: '192.168.4.1',                         label: 'AP 回退地址' },
    { text: 'Notification.show',                   label: 'Notification.show 调用（富文本通知）' },
];

for (const { text, label } of RESTART_HINTS) {
    check(jsContent.includes(text), `${label}`);
}

// 以太网保存函数必须引用 ethernet-save-btn
check(
    jsContent.includes('ethernet-save-btn'),
    'saveEthernetConfig 引用 ethernet-save-btn'
);

// 4G 保存函数必须引用 cellular-save-btn
check(
    jsContent.includes('cellular-save-btn'),
    'saveCellularConfig 引用 cellular-save-btn'
);

// LoRa 保存函数必须引用 lora-save-btn
check(
    jsContent.includes('lora-save-btn'),
    'saveLoRaConfig 引用 lora-save-btn'
);

// 各保存函数调用 _startSaveBtnCountdown
const countdownCallCount = (jsContent.match(/_startSaveBtnCountdown\s*\(/g) || []).length;
// 定义 1 次 + 调用 3 次（ethernet/cellular/lora）= 至少 4 处
check(
    countdownCallCount >= 4,
    `_startSaveBtnCountdown 出现次数 (${countdownCallCount} >= 4，含 1 处定义 + 3 处调用)`
);

// ─── 4. 按钮禁用逻辑验证 ────────────────────────────────────────────────────
console.log('\n[4] 保存按钮禁用逻辑验证');

// 保存时必须有 disabled = true
const disableCount = (jsContent.match(/\.disabled\s*=\s*true/g) || []).length;
check(
    disableCount >= 3,
    `保存按钮禁用 (.disabled = true) 出现 ${disableCount} 次 (>= 3)`
);

// ─── 汇总 ────────────────────────────────────────────────────────────────────
console.log(`\n=== 验证结果: ${checks} 项检查, ${errors} 项失败 ===\n`);

if (errors > 0) {
    process.exit(1);
}
console.log('✅ 网络配置页面 UI 完整性验证通过！\n');
process.exit(0);
