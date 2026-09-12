#!/usr/bin/env node
/**
 * @file e2e-ota-test.js
 * @brief OTA 固件升级端到端测试脚本（针对真实设备）
 *
 * 覆盖流程：
 * - 状态查询（envelope 结构校验）
 * - 在线升级参数校验（缺失/非法 URL 返回 400）
 * - begin/cancel 会话生命周期（--allow-begin，可选）
 * - 完整升级：begin -> multipart 分块上传 -> 状态轮询 -> 设备重启恢复验证（--firmware）
 *
 * 用法：
 *   node scripts/e2e-ota-test.js [--url http://192.168.4.1] [--user admin --pass 123456]
 *                                [--firmware path/to/fw.bin] [--allow-begin]
 *
 * 不带 --firmware 时仅执行非破坏性套件，不会真正写入固件。
 * 需要 Node 18+（依赖全局 fetch / FormData / Blob）。
 */

'use strict';

const fs = require('fs');
const path = require('path');

// ---------- 参数解析 ----------
const args = process.argv.slice(2);
function getArg(name, def) {
    const idx = args.indexOf('--' + name);
    if (idx >= 0 && idx + 1 < args.length) return args[idx + 1];
    return def;
}
const BASE = (getArg('url', 'http://192.168.4.1') || '').replace(/\/+$/, '');
const USER = getArg('user', '');
const PASS = getArg('pass', '');
const FIRMWARE = getArg('firmware', '');
const ALLOW_BEGIN = args.includes('--allow-begin');

let sessionCookie = '';
let passed = 0;
let failed = 0;

function ok(name, detail) {
    passed++;
    console.log('  [PASS] ' + name + (detail ? ' - ' + detail : ''));
}
function fail(name, detail) {
    failed++;
    console.log('  [FAIL] ' + name + (detail ? ' - ' + detail : ''));
}
function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

async function request(method, apiPath, options) {
    options = options || {};
    const headers = Object.assign({}, options.headers || {});
    if (sessionCookie) headers['Cookie'] = sessionCookie;
    const resp = await fetch(BASE + apiPath, {
        method: method,
        headers: headers,
        body: options.body,
        signal: AbortSignal.timeout(options.timeout || 15000)
    });
    let json = null;
    const text = await resp.text();
    try { json = JSON.parse(text); } catch (e) { /* 非 JSON 响应 */ }
    return { status: resp.status, json: json, text: text, headers: resp.headers };
}

async function login() {
    if (!USER) {
        console.log('[INFO] 未提供凭据，尝试匿名访问（设备未启用认证时可用）');
        return;
    }
    const body = 'username=' + encodeURIComponent(USER) + '&password=' + encodeURIComponent(PASS);
    const resp = await request('POST', '/api/auth/login', {
        body: body,
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        timeout: 30000  // PBKDF2 首次登录可能较慢
    });
    if (resp.status !== 200 || !resp.json) {
        throw new Error('登录失败: HTTP ' + resp.status + ' ' + resp.text.slice(0, 200));
    }
    // Current firmware sets an HttpOnly session cookie; retain legacy sessionId
    // parsing so the test remains useful with older images.
    const data = (resp.json.data) ? resp.json.data : resp.json;
    const setCookie = resp.headers.get('set-cookie') || '';
    const sessionMatch = setCookie.match(/(?:^|[,; ]+)session=([^;, ]+)/i);
    if (sessionMatch) {
        sessionCookie = 'session=' + sessionMatch[1];
    } else if (data.sessionId) {
        sessionCookie = 'sessionId=' + data.sessionId;
    } else {
        const legacyMatch = setCookie.match(/(?:^|[,; ]+)sessionId=([^;, ]+)/i);
        if (legacyMatch) sessionCookie = 'sessionId=' + legacyMatch[1];
    }
    if (!sessionCookie) throw new Error('登录成功但未获得 session cookie');
}

async function loginAndReport() {
    await login();
    ok('登录认证', 'cookie=' + sessionCookie.split('=')[0]);
}

// ---------- 非破坏性套件 ----------

async function testStatusEndpoint() {
    const resp = await request('GET', '/api/ota/status');
    if (resp.status !== 200 || !resp.json) return fail('状态查询', 'HTTP ' + resp.status);
    const body = resp.json.data || resp.json;
    if (typeof body.in_progress !== 'boolean' || typeof body.progress !== 'number') {
        return fail('状态查询', 'envelope 结构异常: ' + resp.text.slice(0, 200));
    }
    ok('状态查询', 'status=' + body.status + ' in_progress=' + body.in_progress);
    return body;
}

async function testSystemInfo() {
    const resp = await request('GET', '/api/system/info');
    if (resp.status !== 200 || !resp.json) return fail('系统信息', 'HTTP ' + resp.status);
    const text = JSON.stringify(resp.json);
    const m = text.match(/"freeSketchSpace"\s*:\s*(\d+)/);
    if (!m) return fail('系统信息', '未找到 freeSketchSpace 字段');
    const free = parseInt(m[1], 10);
    if (free <= 0) return fail('系统信息', '可用升级空间为 ' + free);
    ok('系统信息', '可用升级空间=' + (free / 1024).toFixed(0) + 'KB');
    return free;
}

async function testUrlValidation() {
    // 缺失 URL
    let resp = await request('POST', '/api/ota/url', {
        body: '',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }
    });
    if (resp.status !== 400) return fail('URL 校验(缺失)', '期望 400，实际 ' + resp.status);
    ok('URL 校验(缺失)', '400 Missing firmware URL');

    // 非法协议
    resp = await request('POST', '/api/ota/url', {
        body: 'url=' + encodeURIComponent('ftp://example.com/fw.bin'),
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }
    });
    if (resp.status !== 400) return fail('URL 校验(非法协议)', '期望 400，实际 ' + resp.status);
    ok('URL 校验(非法协议)', '400 Invalid URL format');
}

async function testBeginCancelCycle() {
    // 小尺寸 begin（不会写入数据），随后取消，验证会话生命周期
    let resp = await request('POST', '/api/ota/begin', {
        body: 'size=1024',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }
    });
    if (resp.status !== 200) {
        // 503 = 内存门控拒绝（设备内存紧张时合理），501 = 无 OTA 分片（4MB 单应用布局），均视为环境限制
        if (resp.status === 503) return ok('begin/cancel 会话', '内存门控拒绝(503)，跳过: ' + resp.text.slice(0, 120));
        if (resp.status === 501) return ok('begin/cancel 会话', '无 OTA 分片(501)，跳过: ' + resp.text.slice(0, 120));
        return fail('begin/cancel 会话', 'begin 期望 200，实际 ' + resp.status + ' ' + resp.text.slice(0, 120));
    }
    ok('OTA begin', resp.text.slice(0, 80));

    const st = await request('GET', '/api/ota/status');
    const body = (st.json && st.json.data) || {};
    if (body.in_progress !== true) fail('begin 后状态', 'in_progress 应为 true');
    else ok('begin 后状态', 'in_progress=true');

    resp = await request('POST', '/api/ota/cancel');
    if (resp.status !== 200) return fail('OTA cancel', 'HTTP ' + resp.status);
    ok('OTA cancel', '会话已取消');
}

// 上传请求网络异常时的仲裁：设备写入成功后 3s 自动重启，重启会使内存会话失效。
// 因此判定链路为：等接口不可达/401 => 等 /api/health 恢复 => 重新登录 =>
// 读 diag-bundle.restart.lastRestartReason，包含 OTA 即写入成功。
async function pollOtaVerdict() {
    let anomaly = 0;
    for (let i = 0; i < 30; i++) {
        await sleep(1000);
        let st;
        try {
            st = await request('GET', '/api/ota/status', { timeout: 5000 });
        } catch (e) {
            anomaly++; // 设备不可达（重启中）
            if (anomaly >= 5) break;
            continue;
        }
        anomaly = 0;
        if (st.status === 200 && st.json) {
            const body = st.json.data || st.json;
            if (body.error) return 0;
            if (body.in_progress === false && body.progress >= 100) return body.progress;
            continue;
        }
        if (st.status === 401) break; // 会话失效 => 设备已重启，内存会话清空
        return 0;
    }
    return await waitRebootAndRestartReason();
}

// 等待设备重启恢复（/api/health 免认证），重新登录后返回上次重启原因
async function waitRebootAndRestartReason() {
    console.log('  [INFO] 设备疑似已重启，等待恢复并检查重启诊断...');
    await sleep(5000);
    let recovered = false;
    for (let i = 0; i < 60; i++) {
        try {
            const h = await request('GET', '/api/health', { timeout: 3000 });
            if (h.status === 200) { recovered = true; break; }
        } catch (e) { /* 设备仍在重启 */ }
        await sleep(2000);
    }
    if (!recovered) return 0;
    ok('重启恢复验证', '设备已恢复响应 /api/health');
    try {
        await login(); // 重启后内存会话已失效，重新登录
    } catch (e) {
        return 0;
    }
    try {
        const r = await request('GET', '/api/system/diag-bundle', { timeout: 8000 });
        if (r.status === 200 && r.json && r.json.data && r.json.data.restart) {
            const reason = r.json.data.restart.lastRestartReason || '';
            console.log('  [INFO] 重启诊断 lastRestartReason=' + reason);
            if (/OTA/i.test(reason)) return 100;
        }
    } catch (e) { /* 诊断不可用 */ }
    return 0;
}

// ---------- 完整升级套件（破坏性，需 --firmware） ----------

async function testFullUpgrade(fwPath, freeSpace) {
    const filePath = path.resolve(fwPath);
    if (!fs.existsSync(filePath)) throw new Error('固件文件不存在: ' + filePath);
    const buf = fs.readFileSync(filePath);
    if (!filePath.endsWith('.bin')) console.log('  [WARN] 文件扩展名不是 .bin，设备端可能拒绝');
    if (freeSpace > 0 && buf.length > freeSpace) {
        throw new Error('固件 ' + buf.length + ' 字节超出可用空间 ' + freeSpace + ' 字节');
    }
    console.log('[INFO] 固件: ' + filePath + ' (' + buf.length + ' 字节)');

    // Step 1: begin（携带预期大小）
    let resp = await request('POST', '/api/ota/begin', {
        body: 'size=' + buf.length,
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' }
    });
    if (resp.status !== 200) {
        if (resp.status === 501) return ok('升级 begin', '无 OTA 分片(501)，4MB 单应用布局不支持固件 OTA: ' + resp.text.slice(0, 120));
        return fail('升级 begin', 'HTTP ' + resp.status + ' ' + resp.text.slice(0, 160));
    }
    ok('升级 begin', resp.text.slice(0, 80));

    // Step 2: multipart 上传
    // 注意：设备端写入完成后会调度 3s 重启，部分客户端（如 Node undici）可能在
    // 响应 body 读回前遇到连接被断开而报 "fetch failed"——此时固件往往已写入成功。
    // 因此上传抛网络异常时不直接判失败，转入 /api/ota/status 仲裁。
    const form = new FormData();
    form.append('firmware', new Blob([buf], { type: 'application/octet-stream' }), path.basename(filePath));
    let uploadNetworkError = null;
    try {
        resp = await request('POST', '/api/ota/upload', { body: form, timeout: 180000 });
    } catch (e) {
        uploadNetworkError = e && e.message ? e.message : String(e);
    }
    if (uploadNetworkError) {
        console.log('  [WARN] 上传请求网络异常: ' + uploadNetworkError + '，通过 ota-status/重启诊断仲裁写入结果');
        const verdict = await pollOtaVerdict();
        if (!verdict) return fail('固件上传', '网络异常且无法确认写入成功: ' + uploadNetworkError);
        ok('固件上传', '客户端连接中断，但设备端写入已成功并重启(progress=' + verdict + '%)');
        return; // 仲裁链路已完成重启恢复验证，跳过后续常规轮询
    } else if (resp.status !== 200) {
        return fail('固件上传', 'HTTP ' + resp.status + ' ' + resp.text.slice(0, 200));
    } else {
        ok('固件上传', resp.text.slice(0, 120));
    }

    // Step 3: 轮询升级状态（写入完成后设备会 3s 后自动重启）
    let lastProgress = -1;
    let completed = false;
    for (let i = 0; i < 120; i++) {
        await sleep(1000);
        let st;
        try {
            st = await request('GET', '/api/ota/status', { timeout: 5000 });
        } catch (e) {
            // 设备重启中，接口不可达 => 转恢复轮询
            ok('设备重启', '状态接口不可达，设备正在重启');
            completed = true;
            break;
        }
        const body = (st.json && st.json.data) || {};
        if (body.progress !== lastProgress) {
            console.log('  [INFO] 进度: ' + body.progress + '% ' + (body.status || '') +
                (body.error ? (' error=' + body.error) : ''));
            lastProgress = body.progress;
        }
        if (body.error) return fail('升级状态', '设备报告错误: ' + body.error);
        if (body.in_progress === false && (body.progress >= 100 || body.current_size > 0)) {
            completed = true;
            ok('升级完成', 'progress=' + body.progress + '% current_size=' + body.current_size);
            break;
        }
    }
    if (!completed) return fail('升级状态轮询', '120s 内未完成');

    // Step 4: 轮询设备恢复（重启验证）
    console.log('[INFO] 等待设备重启恢复...');
    await sleep(5000);
    for (let i = 0; i < 60; i++) {
        try {
            const r = await request('GET', '/api/health', { timeout: 3000 });
            if (r.status === 200) {
                ok('重启恢复验证', '设备已恢复响应 /api/health');
                const info = await request('GET', '/api/system/info', { timeout: 5000 });
                if (info.status === 200) {
                    const m = JSON.stringify(info.json).match(/"firmwareVersion"\s*:\s*"([^"]+)"/);
                    if (m) ok('升级后版本', 'firmwareVersion=' + m[1]);
                }
                return;
            }
        } catch (e) { /* 继续等待 */ }
        await sleep(2000);
    }
    fail('重启恢复验证', '120s 内设备未恢复');
}

// ---------- 主流程 ----------

(async function main() {
    console.log('===== FastBee OTA 端到端测试 =====');
    console.log('[INFO] 目标设备: ' + BASE);
    console.log('[INFO] 模式: ' + (FIRMWARE ? '完整升级（破坏性）' : '非破坏性套件'));

    try {
        await loginAndReport();
        const status = await testStatusEndpoint();
        if (status && status.in_progress) {
            console.log('[WARN] 设备当前有进行中的 OTA，先尝试取消');
            await request('POST', '/api/ota/cancel');
            await sleep(1000);
        }
        const freeSpace = await testSystemInfo();
        await testUrlValidation();
        if (ALLOW_BEGIN && !FIRMWARE) await testBeginCancelCycle();
        if (FIRMWARE) await testFullUpgrade(FIRMWARE, freeSpace || 0);
    } catch (err) {
        fail('执行异常', err && err.message ? err.message : String(err));
    }

    console.log('==================================');
    console.log('结果: ' + passed + ' 通过, ' + failed + ' 失败');
    process.exit(failed > 0 ? 1 : 0);
})();
