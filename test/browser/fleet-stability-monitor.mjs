#!/usr/bin/env node
/**
 * FastBee 舰队长期稳定性监测器（Playwright 浏览器自动化）
 *
 * 对舰队 5 台设备持续执行真实用户视角的 Web 全链路验证：
 *   1. HTTP 探针（显式 STA 地址）→ 在线率 / 响应时间
 *   2. 浏览器打开主页 → 登录 → 验证 #app-container 显示
 *   3. 遍历侧边栏全部菜单页，验证每页内容非空、无 console/pageerror/HTTP 4xx/5xx
 *   4. 采样 /api/system/status（内部 DRAM/uptime/features）→ 内存趋势 + 重启检测
 *   5. 每周期追加 JSONL 记录；周期性生成 fleet-stability-report.md
 *
 * 用法:
 *   node fleet-stability-monitor.mjs                     # 无限循环，间隔 180s
 *   node fleet-stability-monitor.mjs --once              # 单轮冒烟
 *   node fleet-stability-monitor.mjs --interval 120 --report-every 8
 *   node fleet-stability-monitor.mjs --max-cycles 40
 *   node fleet-stability-monitor.mjs --devices esp32c3-f4r0,esp32s3-f16r8
 *   node fleet-stability-monitor.mjs --min-duration-hours 8 --max-cycles 161
 *   node fleet-stability-monitor.mjs --report-only       # 仅基于已有 JSONL 重新生成报告
 *
 * 数据输出: .pio/fleet-monitor/<run-id>/<device>/cycles.jsonl + events.jsonl
 * 报告输出: .pio/fleet-monitor/<run-id>/fleet-stability-report.md
 */

import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..', '..');
// ─── 舰队定义 ─────────────────────────────────────────
// Keep the test bench's STA addresses explicit.  AP addresses and stale
// mDNS aliases are intentionally excluded so a monitor cycle cannot route
// through a device hotspot or attribute one board's result to another.
const DEVICES = [
  { label: 'esp32c3-f4r0', host: '192.168.5.55', ips: ['192.168.5.55'], model: 'ESP32-C3-F4R0' },
  { label: 'esp32c6-f4r0', host: '192.168.5.19', ips: ['192.168.5.19'], model: 'ESP32-C6-F4R0' },
  { label: 'esp32-f4r0', host: '192.168.5.21', ips: ['192.168.5.21'], model: 'ESP32-F4R0' },
  { label: 'esp32s3-f16r8', host: '192.168.5.22', ips: ['192.168.5.22'], model: 'ESP32-S3-F16R8' },
  { label: 'esp32s3-f8r0', host: '192.168.5.67', ips: ['192.168.5.67'], model: 'ESP32-S3-F8R0' },
];

const AUTH = {
  username: process.env.FASTBEE_WEB_USERNAME || 'admin',
  password: process.env.FASTBEE_WEB_PASSWORD || 'admin123',
};
// WebMemoryGuard 阈值（include/core/MemoryBudget.h）：free8<20480 触发 503，恢复需 >=21504
const RISK_GUARD_TRIGGER = 20480;
const RISK_GUARD_RECOVERY = 21504;
const RISK_WATCH = 30720; // 观察线：低于此值视为内存紧张
// 中等压力仍要覆盖菜单，只把页面间隔放慢；否则 C3/F4 的正常稳态水位
// 会让整个长稳周期都跳过真实浏览器验收。只有进入 WebMemoryGuard 的
// 实际危险区才跳过一轮，等待设备恢复。
const GENTLE_CYCLE_HEAP = 45000;
const GENTLE_CYCLE_MAX_ALLOC = 12288;
const HARD_SKIP_HEAP = RISK_GUARD_TRIGGER;
const HARD_SKIP_MAX_ALLOC = 4096;
const MENU_DELAY_NORMAL = 1500;
const MENU_DELAY_GENTLE = 3000;
const NAVIGATION_ABORT_WINDOW_MS = 10_000;
const ACCEPT_MIN_HEAP = 25_600;
const ACCEPT_MIN_MAX_ALLOC = 8_000;
const ACCEPT_MAX_HEAP_DROP = 20_000;
const ACCEPT_MAX_ALLOC_DROP = 12_000;
const ACCEPT_MAX_PROBE_P95_MS = 5_000;
const ACCEPT_MAX_MENU_P95_MS = 7_000;

function runId() {
  return new Date().toISOString().replace(/[:.]/g, '-');
}

// ─── CLI 参数 ─────────────────────────────────────────
function parseArgs() {
  const argv = process.argv.slice(2);
  const opts = {
    interval: 180,       // 每设备周期间隔（秒）
    reportEvery: 8,      // 每 N 周期重新生成报告
    maxCycles: 0,        // 0 = 无限
    once: false,
    reportOnly: false,
    devices: null,       // 逗号分隔 label 过滤
    cycleTimeout: 420_000, // 单设备单周期看门狗 7 分钟（含 goto/登录重试预算）
    minDurationHours: 0,
    allowExistingOutput: false,
    outputDir: path.join(ROOT, '.pio', 'fleet-monitor', runId()),
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === '--once') opts.once = true;
    else if (a === '--report-only') opts.reportOnly = true;
    else if (a === '--interval') opts.interval = parseInt(argv[++i], 10);
    else if (a === '--report-every') opts.reportEvery = parseInt(argv[++i], 10);
    else if (a === '--max-cycles') opts.maxCycles = parseInt(argv[++i], 10);
    else if (a === '--devices') opts.devices = argv[++i].split(',').map(s => s.trim());
    else if (a === '--cycle-timeout') opts.cycleTimeout = parseInt(argv[++i], 10) * 1000;
    else if (a === '--min-duration-hours') opts.minDurationHours = Number(argv[++i]);
    else if (a === '--allow-existing-output') opts.allowExistingOutput = true;
    else if (a === '--output-dir') {
      const value = argv[++i];
      if (!value) throw new Error('--output-dir requires a path');
      opts.outputDir = path.isAbsolute(value) ? value : path.resolve(ROOT, value);
    }
  }
  if (opts.once) opts.maxCycles = 1;
  if (!Number.isFinite(opts.interval) || opts.interval < 15) throw new Error('--interval must be at least 15 seconds');
  if (!Number.isFinite(opts.reportEvery) || opts.reportEvery < 1) throw new Error('--report-every must be at least 1');
  if (!Number.isFinite(opts.maxCycles) || opts.maxCycles < 0) throw new Error('--max-cycles must be zero or greater');
  if (!Number.isFinite(opts.cycleTimeout) || opts.cycleTimeout < 30_000) throw new Error('--cycle-timeout must be at least 30 seconds');
  if (!Number.isFinite(opts.minDurationHours) || opts.minDurationHours < 0) throw new Error('--min-duration-hours must be zero or greater');
  return opts;
}
const OPTS = parseArgs();
const OUT_DIR = OPTS.outputDir;
const REPORT_PATH = path.join(OUT_DIR, 'fleet-stability-report.md');
const DEVICES_ACTIVE = OPTS.devices
  ? DEVICES.filter(d => OPTS.devices.includes(d.label))
  : DEVICES;
if (DEVICES_ACTIVE.length === 0) throw new Error('no matching devices selected');

// ─── 工具函数 ─────────────────────────────────────────
const sleep = (ms) => new Promise(r => setTimeout(r, ms));
const nowIso = () => new Date().toISOString();
const slug = (s) => s.replace(/[^a-zA-Z0-9]+/g, '-');
function log(msg) {
  const line = `[${nowIso()}] ${msg}`;
  console.log(line);
  fs.appendFileSync(path.join(OUT_DIR, 'monitor.log'), line + '\n', 'utf8');
}
function appendJsonl(file, obj) {
  fs.appendFileSync(file, JSON.stringify(obj) + '\n', 'utf8');
}
function readJsonl(file) {
  if (!fs.existsSync(file)) return [];
  return fs.readFileSync(file, 'utf8').split('\n')
    .filter(l => l.trim()).map(l => { try { return JSON.parse(l); } catch { return null; } })
    .filter(Boolean);
}
function pct(n, d) { return d > 0 ? (100 * n / d) : 0; }
function percentile(arr, p) {
  if (!arr.length) return 0;
  const s = [...arr].sort((a, b) => a - b);
  return s[Math.min(s.length - 1, Math.floor(p * s.length))];
}
function trendDrop(arr) {
  if (arr.length < 5) return 0;
  const windowSize = Math.max(1, Math.floor(arr.length * 0.2));
  const firstMedian = percentile(arr.slice(0, windowSize), 0.5);
  const lastMedian = percentile(arr.slice(-windowSize), 0.5);
  return firstMedian - lastMedian;
}
function assertFreshOutput() {
  if (OPTS.allowExistingOutput || !fs.existsSync(OUT_DIR)) return;
  const markers = [
    path.join(OUT_DIR, 'run-metadata.json'),
    path.join(OUT_DIR, 'monitor.log'),
    path.join(OUT_DIR, 'fleet-stability-report.md'),
    ...DEVICES_ACTIVE.flatMap(dev => [
      path.join(OUT_DIR, slug(dev.label), 'cycles.jsonl'),
      path.join(OUT_DIR, slug(dev.label), 'events.jsonl'),
    ]),
  ];
  const occupied = markers.find(file => fs.existsSync(file) && fs.statSync(file).size > 0);
  if (occupied) {
    throw new Error(`output directory already contains monitor data: ${occupied}; use a new directory or --allow-existing-output`);
  }
}
async function timedFetch(url, timeoutMs = 10_000, { requireJson = false } = {}) {
  const t0 = Date.now();
  try {
    const resp = await fetch(url, { signal: AbortSignal.timeout(timeoutMs) });
    const rtt = Date.now() - t0;
    const text = await resp.text();
    const result = { ok: resp.ok, status: resp.status, rtt, text };
    if (!resp.ok) return result;
    if (!text.trim()) {
      return { ...result, ok: false, error: 'empty response body' };
    }
    if (requireJson) {
      try {
        result.json = JSON.parse(text);
      } catch {
        return { ...result, ok: false, error: 'invalid JSON response' };
      }
    }
    return result;
  } catch (e) {
    return { ok: false, status: 0, rtt: Date.now() - t0, error: String(e?.cause?.code || e?.message || e) };
  }
}

// ─── HTTP 探针：STA 地址优先，两轮重试 ───────────────
// 经典 ESP32 低堆时偶发连接复位（000），因此做两轮后再判定离线。
// 去重可避免 host 本身就是 IP 时被重复访问并误记为 mDNS 命中。
async function probeDevice(dev) {
  const hostVia = /^\d{1,3}(?:\.\d{1,3}){3}$/.test(dev.host) ? 'ip' : 'mdns';
  const candidates = [...new Map([
    { via: hostVia, base: `http://${dev.host}` },
    ...dev.ips.map(ip => ({ via: 'ip', base: `http://${ip}` })),
  ].map(candidate => [candidate.base, candidate])).values()];
  let attempts = 0;
  const transientFailures = [];
  for (let round = 0; round < 2; round++) {
    for (const c of candidates) {
      attempts++;
      const r = await timedFetch(`${c.base}/api/health`, 10_000, { requireJson: true });
      if (r.ok) return { ok: true, base: c.base, via: c.via, rtt: r.rtt, status: r.status, attempts, transientFailures };
      transientFailures.push({ via: c.via, status: r.status, error: r.error || `HTTP ${r.status}` });
    }
    if (round === 0) await sleep(3_000);
  }
  return { ok: false, base: `http://${dev.host}`, via: 'none', rtt: 0, error: 'unreachable', attempts, transientFailures };
}

// ─── 内存采样（/api/system/status 公开端点）───────────
async function sampleMemory(base) {
  const transientFailures = [];
  let r = null;
  for (let attempt = 1; attempt <= 2; attempt++) {
    r = await timedFetch(`${base}/api/system/status`, 10_000, { requireJson: true });
    if (r.ok && r.json?.data && typeof r.json.data === 'object') break;
    if (r.ok) {
      r = { ...r, ok: false, error: 'invalid system status payload' };
    }
    transientFailures.push({
      attempt,
      status: r.status,
      error: r.error || `HTTP ${r.status}`,
    });
    if (attempt === 1) await sleep(1_000);
  }
  if (!r.ok) {
    return {
      freeHeap: null,
      maxAlloc: null,
      uptime: null,
      rssi: null,
      noMemoryFeatures: null,
      rtt: r.rtt,
      attempts: transientFailures.length,
      transientFailures,
      status503: transientFailures.some(failure => failure.status === 503),
      error: r.error || (r.status ? `HTTP ${r.status}` : 'request failed'),
    };
  }
  const data = r.json.data;
  const features = data.features || {};
  const noMemory = Object.values(features).filter(v => v === 'no-memory').length;
  const memory = data.memory || {};
  return {
    // S3 的 freeHeap/heapMaxAlloc 可能包含 PSRAM；WebMemoryGuard 依据的
    // 是内部 8-bit DRAM。兼容尚未提供新字段的旧固件。
    freeHeap: Number(memory.dram8Free ?? data.freeHeap ?? 0),
    maxAlloc: Number(memory.dram8Largest ?? memory.heapMaxAlloc ?? 0),
    uptime: Number(data.uptime || 0),
    rssi: data.rssi ?? null,
    noMemoryFeatures: noMemory,
    rtt: r.rtt,
    attempts: transientFailures.length + 1,
    transientFailures,
    status503: transientFailures.some(failure => failure.status === 503),
  };
}

// ─── 浏览器轮：登录 + 菜单遍历 + 错误采集 ─────────────
async function browserRound(dev, context, base, menuDelay = MENU_DELAY_NORMAL, isAborted = () => false) {
  const round = {
    login: { ok: false, ms: 0, error: null },
    visibleMenus: [],
    menus: [],
    consoleErrors: [],
    pageErrors: [],
    httpErrors: [],       // status >= 400 或 requestfailed
    webguard503: 0,       // WebMemoryGuard 拒绝计数
    domErrorToasts: 0,
    navigationRetries: 0,
    interactionRetries: 0,
    ignoredSession401: 0,
    ignoredNavigationAborts: 0,
  };
  const page = await context.newPage();
  let closing = false;
  let authenticated = false;
  let navigationAbortUntil = 0;
  let pageGeneration = 0;
  const requestGenerations = new WeakMap();
  page.setDefaultTimeout(30_000);
  page.on('request', req => requestGenerations.set(req, pageGeneration));
  page.on('console', m => {
    if (m.type() === 'error') round.consoleErrors.push(m.text().slice(0, 300));
  });
  page.on('pageerror', e => round.pageErrors.push(String(e).slice(0, 300)));
  page.on('requestfailed', req => {
    if (closing) return;
    const url = req.url();
    const failure = req.failure()?.errorText || '';
    const requestGeneration = requestGenerations.get(req);
    const intentionalPageAbort = failure === 'net::ERR_ABORTED' &&
      Date.now() <= navigationAbortUntil &&
      Number.isInteger(requestGeneration) && requestGeneration < pageGeneration &&
      ['fetch', 'xhr'].includes(req.resourceType()) &&
      url.startsWith(`${base}/api/`);
    if (intentionalPageAbort) {
      round.ignoredNavigationAborts++;
      return;
    }
    if (!url.includes('favicon')) round.httpErrors.push(`failed ${req.method()} ${url.slice(0, 120)} ${failure}`);
  });
  page.on('response', resp => {
    const expectedSessionProbe = !authenticated && resp.status() === 401 &&
      resp.request().method() === 'GET' &&
      resp.url().startsWith(`${base}/api/auth/session`);
    if (expectedSessionProbe) {
      round.ignoredSession401++;
      return;
    }
    if (resp.status() === 503) {
      round.webguard503++;
      round.httpErrors.push(`503 ${resp.request().method()} ${resp.url().slice(0, 120)}`);
    } else if (resp.status() >= 400 && !resp.url().includes('favicon')) {
      round.httpErrors.push(`${resp.status()} ${resp.request().method()} ${resp.url().slice(0, 120)}`);
    }
  });
  page.on('dialog', d => d.accept().catch(() => {}));

  const t0 = Date.now();
  try {
    // Arm the session probe before navigation. The login page is visible in
    // the initial HTML, so inspecting the DOM at DOMContentLoaded races with
    // cookie-session restoration on constrained devices.
    const sessionProbePromise = page.waitForResponse(response =>
      response.request().method() === 'GET' &&
      response.url().startsWith(`${base}/api/auth/session`),
    { timeout: 45_000 }).catch(() => null);

    // 1) 打开首页（ESP32 单连接串行传输，高负载时首屏可达 30~60s，带重试放宽到 90s）
    for (let gotoRetry = 0; ; gotoRetry++) {
      try {
        await page.goto(`${base}/`, { waitUntil: 'domcontentloaded', timeout: 90_000 });
        break;
      } catch (gotoErr) {
        if (gotoRetry >= 1) throw gotoErr;
        round.navigationRetries++;
        await sleep(5_000);
      }
    }
    // 2) Wait for the asynchronous session check before deciding whether the
    // login form is real. A valid reused session must go straight to the app.
    const sessionResponse = await sessionProbePromise;
    let sessionValid = false;
    if (sessionResponse?.ok()) {
      const payload = await sessionResponse.json().catch(() => null);
      sessionValid = Boolean(payload?.success && payload?.data?.sessionValid);
    }
    if (sessionValid) {
      await page.waitForSelector('#app-container', { state: 'visible', timeout: 45_000 });
    } else if (!sessionResponse) {
      // Preserve a bounded fallback for older firmware that does not expose
      // the session endpoint, while allowing its boot script to settle first.
      await page.waitForFunction(() => window.__fastbeeBootReady === true, { timeout: 40_000 }).catch(() => {});
      await sleep(250);
    }
    const needLogin = sessionValid ? false : await page.evaluate(() => {
      const lp = document.querySelector('#login-page');
      const app = document.querySelector('#app-container');
      if (app && !app.classList.contains('fb-hidden')) return false;
      if (lp && lp.style.display !== 'none') return true;
      return true;
    });
    if (needLogin) {
      await page.waitForSelector('#login-page', { state: 'visible', timeout: 30_000 });
      // 等待 boot chunks 就绪，避免内联 fallback 整页重载
      await page.waitForFunction(() => window.__fastbeeBootReady === true, { timeout: 40_000 }).catch(() => {});
      // 嵌入式设备偶发输入卡顿：fill/click 失败自动重试一次
      for (let fi = 0; ; fi++) {
        try {
          await page.fill('#username', AUTH.username);
          await page.fill('#password', AUTH.password);
          await page.click('#login-button');
          break;
        } catch (fillErr) {
          if (fi >= 1) throw fillErr;
          round.interactionRetries++;
          await sleep(3_000);
        }
      }
      await page.waitForSelector('#app-container', { state: 'visible', timeout: 45_000 });
      await page.waitForFunction(() => {
        const lp = document.querySelector('#login-page');
        return !lp || lp.style.display === 'none';
      }, { timeout: 15_000 }).catch(() => {});
    } else {
      await page.waitForSelector('#app-container', { state: 'visible', timeout: 20_000 });
    }
    round.login.ok = true;
    round.login.ms = Date.now() - t0;
    authenticated = true;

    // 自适应等待：modals 片段 / 首屏数据
    await page.waitForFunction(() => document.querySelector('.modal') !== null, { timeout: 10_000 }).catch(() => {});
    await sleep(1500);

    // 3) 枚举侧边栏可见菜单（前端按运行时能力位隐藏不支持项，display:none 不计入覆盖）
    const menuPages = await page.$$eval('.menu-item[data-page]', els =>
      els.filter(e => getComputedStyle(e).display !== 'none')
         .map(e => e.getAttribute('data-page')).filter(Boolean));
    round.visibleMenus = [...new Set(menuPages)];

    for (const p of menuPages) {
      if (isAborted()) throw new Error('cycle aborted');
      const m = { page: p, ok: false, ms: 0, error: null };
      const t1 = Date.now();
      try {
        // 侧边栏溢出视口的菜单项先滚动入视口，再点击（C6 实测 rule-script/data 在折叠区）
        await page.locator(`.menu-item[data-page="${p}"]`).scrollIntoViewIfNeeded({ timeout: 5_000 }).catch(() => {});
        // Only requests already in flight before this click may be classified
        // as intentional page-navigation cancellation. Requests started by
        // the new page remain real failures even inside the short time window.
        pageGeneration++;
        navigationAbortUntil = Date.now() + NAVIGATION_ABORT_WINDOW_MS;
        await page.click(`.menu-item[data-page="${p}"]`, { timeout: 25_000 });
        // 页面容器有内容视为渲染成功
        await page.waitForFunction((sel) => {
          const el = document.querySelector(sel);
          return !!el && (el.children.length > 0 || (el.textContent || '').trim().length > 0);
        }, `#${p}-page`, { timeout: 20_000 });
        m.ok = true;
      } catch (e) {
        m.error = String(e?.message || e).split('\n')[0].slice(0, 200);
      }
      m.ms = Date.now() - t1;
      round.menus.push(m);
      if (isAborted()) throw new Error('cycle aborted');
      await sleep(menuDelay); // 嵌入式设备串行处理，给设备喘息
    }

    // 4) DOM 错误提示扫描（notification/toast 中的错误项）
    round.domErrorToasts = await page.evaluate(() => {
      const sels = '.notification-error, .toast-error, .message-error, .alert-error';
      return Array.from(document.querySelectorAll(sels))
        .filter(el => el.offsetParent !== null).length;
    }).catch(() => 0);

    // 5) 回到 dashboard 收尾
    pageGeneration++;
    navigationAbortUntil = Date.now() + NAVIGATION_ABORT_WINDOW_MS;
    await page.click('.menu-item[data-page="dashboard"]', { timeout: 10_000 }).catch(() => {});
  } catch (e) {
    round.login.error = String(e?.message || e).split('\n')[0].slice(0, 300);
  } finally {
    closing = true;
    await page.close().catch(() => {});
  }
  // 截断错误列表防止 JSONL 膨胀
  // Chromium also emits a generic console error for the expected unauthenticated
  // session probe. Remove at most the number of responses classified above;
  // any other 401 remains visible as a real authentication regression.
  let expected401Console = round.ignoredSession401;
  round.consoleErrors = round.consoleErrors.filter(message => {
    if (expected401Console > 0 && /status of 401|401 \(Unauthorized\)/i.test(message)) {
      expected401Console--;
      return false;
    }
    return true;
  });
  round.consoleErrors = round.consoleErrors.slice(0, 20);
  round.pageErrors = round.pageErrors.slice(0, 10);
  round.httpErrors = round.httpErrors.slice(0, 20);
  return round;
}

// ─── 单设备监测循环 ───────────────────────────────────
async function monitorDevice(dev, browserHolder, stopFlag) {
  const dir = path.join(OUT_DIR, slug(dev.label));
  fs.mkdirSync(dir, { recursive: true });
  const cyclesFile = path.join(dir, 'cycles.jsonl');
  const eventsFile = path.join(dir, 'events.jsonl');

  let cycle = 0;
  let context = null;
  let contextBase = null;
  let lastUptime = null;
  let wasOnline = null;
  let consecutiveOffline = 0;
  let lastWebguard503 = 0; // 上轮 503 计数，驱动本轮降载

  const ensureContext = async (base) => {
    if (context && !browserHolder.browser.isConnected()) context = null;
    if (!context) {
      context = await browserHolder.browser.newContext({
        viewport: { width: 1366, height: 800 },
        extraHTTPHeaders: { 'Accept-Language': 'zh-CN' },
      });
    }
    contextBase = base;
    return context;
  };
  const dropContext = async () => {
    if (!context) return;
    const closingContext = context;
    const logoutBase = contextBase;
    context = null;
    contextBase = null;
    // Browser contexts own independent cookie sessions. Explicitly revoke the
    // monitor's session so repeated validation runs do not occupy the device's
    // bounded server-side session table until its one-hour expiry.
    if (logoutBase) {
      await closingContext.request.post(`${logoutBase}/api/auth/logout`, { timeout: 5_000 }).catch(() => {});
    }
    await closingContext.close().catch(() => {});
  };
  const observeUptime = (sample, rec, currentCycle) => {
    const uptime = sample?.uptime;
    if (!Number.isFinite(uptime) || uptime <= 0) return false;
    const rebooted = lastUptime !== null && uptime < lastUptime - 5000;
    if (rebooted) {
      rec.reboot = true;
      appendJsonl(eventsFile, { t: nowIso(), type: 'reboot', cycle: currentCycle, uptimeWas: lastUptime, uptimeNow: uptime });
      log(`${dev.label} #${currentCycle} ⚠ 检测到重启 (uptime ${lastUptime}→${uptime})`);
    }
    lastUptime = uptime;
    return rebooted;
  };

  while (!stopFlag.value) {
    cycle++;
    const cycleStart = Date.now();
    const rec = { t: nowIso(), cycle, device: dev.label, probe: null, round: null, mem: null, reboot: false, browserRetries: 0, error: null };
    let cycleTimedOut = false;

    const runOnce = (async () => {
      // 1) HTTP 探针
      const probe = await probeDevice(dev);
      rec.probe = probe;
      if (cycleTimedOut) return;
      if (!probe.ok) {
        consecutiveOffline++;
        if (wasOnline !== false) appendJsonl(eventsFile, { t: nowIso(), type: 'offline', consecutive: consecutiveOffline });
        wasOnline = false;
        log(`${dev.label} #${cycle} OFFLINE (连续 ${consecutiveOffline} 次)`);
        return;
      }
      if (wasOnline === false) appendJsonl(eventsFile, { t: nowIso(), type: 'online-restored', after: consecutiveOffline });
      wasOnline = true;
      consecutiveOffline = 0;

      // 2) 内存采样（周期首尾各一次，取尾部为当前水位）
      rec.mem = await sampleMemory(probe.base);
      if (cycleTimedOut) return;
      if (observeUptime(rec.mem, rec, cycle)) await dropContext(); // session 失效

      // 3) 浏览器轮：中等压力使用温和节奏，只有 Web 护栏危险区或
      // 上轮已出现 503 时才跳过一轮。这样受限设备仍有完整菜单覆盖。
      const heapNow = rec.mem?.freeHeap ?? Infinity;
      const maxAllocNow = rec.mem?.maxAlloc ?? Infinity;
      const gentle = heapNow < GENTLE_CYCLE_HEAP || maxAllocNow < GENTLE_CYCLE_MAX_ALLOC;
      if (rec.mem?.status503) {
        rec.skipped = 'system-status-503';
        log(`${dev.label} #${cycle} 降载：system status 返回 503，本轮跳过浏览器轮`);
      } else if (heapNow < HARD_SKIP_HEAP) {
        rec.skipped = 'web-guard-low-heap';
        log(`${dev.label} #${cycle} 降载：heap=${heapNow} < ${HARD_SKIP_HEAP}，本轮跳过浏览器轮`);
      } else if (maxAllocNow < HARD_SKIP_MAX_ALLOC) {
        rec.skipped = 'web-guard-fragmented';
        log(`${dev.label} #${cycle} 降载：maxAlloc=${maxAllocNow} < ${HARD_SKIP_MAX_ALLOC}（危险碎片化），本轮跳过浏览器轮`);
      } else if (lastWebguard503 > 0) {
        rec.skipped = 'cooldown-503';
        log(`${dev.label} #${cycle} 降载：上轮 ${lastWebguard503} 次 503，本轮冷却跳过浏览器轮`);
      } else {
        for (let attempt = 0; attempt < 2; attempt++) {
          if (cycleTimedOut) break;
          try {
            const ctx = await ensureContext(probe.base);
            const previousRound = rec.round;
            const attemptRound = await browserRound(
              dev,
              ctx,
              probe.base,
              gentle ? MENU_DELAY_GENTLE : MENU_DELAY_NORMAL,
              () => cycleTimedOut,
            );
            if (previousRound) {
              attemptRound.consoleErrors = [...previousRound.consoleErrors, ...attemptRound.consoleErrors].slice(0, 20);
              attemptRound.pageErrors = [...previousRound.pageErrors, ...attemptRound.pageErrors].slice(0, 10);
              attemptRound.httpErrors = [...previousRound.httpErrors, ...attemptRound.httpErrors].slice(0, 20);
              attemptRound.webguard503 += previousRound.webguard503 || 0;
              attemptRound.domErrorToasts += previousRound.domErrorToasts || 0;
              attemptRound.navigationRetries += previousRound.navigationRetries || 0;
              attemptRound.interactionRetries += previousRound.interactionRetries || 0;
              attemptRound.ignoredSession401 += previousRound.ignoredSession401 || 0;
              attemptRound.ignoredNavigationAborts += previousRound.ignoredNavigationAborts || 0;
            }
            rec.round = attemptRound;
            if (cycleTimedOut) break;
            if (rec.round.login.ok) break;
            if (attempt === 0) {
              rec.browserRetries++;
              log(`${dev.label} #${cycle} 登录失败，休眠 10s 后重试: ${rec.round.login.error}`);
              await dropContext();
              await sleep(10_000);
              if (cycleTimedOut) break;
            }
          } catch (e) {
            rec.error = String(e?.message || e).slice(0, 300);
            await dropContext();
            log(`${dev.label} #${cycle} 浏览器轮异常: ${rec.error}`);
            break;
          }
        }
      }
      if (rec.round) {
        lastWebguard503 = rec.round.webguard503 || 0;
        const menusOk = rec.round.menus.filter(m => m.ok).length;
        log(`${dev.label} #${cycle} probe=${probe.via} ${probe.rtt}ms | login=${rec.round.login.ok ? rec.round.login.ms + 'ms' : 'FAIL'} | menus=${menusOk}/${rec.round.menus.length} | heap=${rec.mem?.freeHeap ?? '?'} | 503=${rec.round.webguard503} cerr=${rec.round.consoleErrors.length}`);
      } else {
        lastWebguard503 = 0; // 降载轮不给设备加压，下轮恢复尝试
      }

      // 4) 周期末再采样一次内存（反映浏览压力后的水位）
      if (!cycleTimedOut) {
        const memAfter = await sampleMemory(probe.base);
        if (memAfter) {
          rec.memAfter = memAfter;
          if (observeUptime(memAfter, rec, cycle)) await dropContext();
        }
      }
      const memory503 = Number(Boolean(rec.mem?.status503)) + Number(Boolean(rec.memAfter?.status503));
      lastWebguard503 = (rec.round?.webguard503 || 0) + memory503;
    })();

    // 看门狗：清理 timer，并等待被关闭的浏览器轮真正退出，避免旧周期
    // 在下一周期继续修改 rec 或创建新 context。
    let watchdog = null;
    try {
      await Promise.race([runOnce, new Promise((_, reject) => {
        watchdog = setTimeout(() => {
          cycleTimedOut = true;
          reject(new Error('cycle timeout'));
        }, OPTS.cycleTimeout);
      })]);
    } catch (e) {
      rec.error = rec.error || `watchdog: ${e.message}`;
      await dropContext();
      await runOnce.catch(() => {});
      log(`${dev.label} #${cycle} 周期超时/异常: ${rec.error}`);
    } finally {
      if (watchdog) clearTimeout(watchdog);
    }

    rec.durationMs = Date.now() - cycleStart;
    appendJsonl(cyclesFile, rec);
    if (OPTS.maxCycles > 0 && cycle >= OPTS.maxCycles) break;
    if (stopFlag.value) break;

    const elapsed = (Date.now() - cycleStart) / 1000;
    const wait = Math.max(15, OPTS.interval - elapsed);
    await sleep(wait * 1000);
  }
  await dropContext();
}

// ─── 报告生成 ─────────────────────────────────────────
function generateReport(extraReasons = []) {
  const t = nowIso();
  const L = []; // 行缓冲
  L.push('# FastBee 舰队 Web 稳定性测试报告');
  L.push('');
  L.push(`> 生成时间：${t}　|　监测方式：Playwright 无头浏览器（真实登录 + 菜单遍历）+ HTTP 探针`);
  L.push('');

  // 汇总表
  L.push('## 舰队总览');
  L.push('');
  L.push('| 设备 | 型号 | 周期数 | 在线率 | 登录通过率 | 菜单通过率 | 重启 | 503(WebGuard) | 最新 heap |');
  L.push('|------|------|-------:|-------:|-----------:|-----------:|-----:|--------------:|----------:|');

  const sections = [];
  const summaries = [];
  for (const dev of DEVICES_ACTIVE) {
    const dir = path.join(OUT_DIR, slug(dev.label));
    const cycles = readJsonl(path.join(dir, 'cycles.jsonl'));
    const events = readJsonl(path.join(dir, 'events.jsonl'));
    const s = summarize(dev, cycles, events);
    summaries.push({ dev, summary: s });
    L.push(`| ${dev.label} | ${dev.model} | ${s.total} | ${s.onlineRate.toFixed(1)}% | ${s.loginRate.toFixed(1)}% | ${s.menuRate.toFixed(1)}% | ${s.reboots} | ${s.webguard503} | ${s.lastHeap ?? '-'} |`);
    sections.push(renderDeviceSection(dev, s));
  }
  L.push('');
  const verdict = evaluateAcceptance(summaries);
  verdict.reasons.push(...extraReasons);
  verdict.ok = verdict.reasons.length === 0;
  L.push('## 自动验收判定');
  L.push('');
  L.push(`**${verdict.ok ? 'PASS' : 'FAIL'}**`);
  L.push('');
  L.push(`- 要求的最短监测窗口：${OPTS.minDurationHours || 0} 小时`);
  if (verdict.reasons.length) {
    for (const reason of verdict.reasons) L.push(`- ${reason}`);
  } else {
    L.push('- 所选设备的在线、登录、全部可见菜单、资源请求、内存和重启指标均满足门禁。');
  }
  L.push('');
  L.push(...sections);
  L.push('---');
  L.push('');
  L.push('## 稳定性评估方法');
  L.push('');
  L.push('- **在线率** = HTTP 探针成功周期 / 总周期（仅使用明确的 STA 地址，不连接设备 AP）');
  L.push('- **登录通过率** = 浏览器成功登录并显示 #app-container 的周期 / 在线周期');
  L.push('- **菜单通过率** = 内容非空渲染成功的菜单次数 / 菜单点击总次数');
  L.push(`- **内存风险线**：freeHeap < ${RISK_GUARD_TRIGGER}B 触发 WebGuard 503；< ${RISK_GUARD_RECOVERY}B 无法恢复；< ${RISK_WATCH}B 为观察线`);
  L.push('- **重启检测**：uptime 回退即记一次重启事件');
  L.push('');
  fs.writeFileSync(REPORT_PATH, L.join('\n'), 'utf8');
  fs.writeFileSync(path.join(OUT_DIR, 'fleet-stability-verdict.json'), JSON.stringify(verdict, null, 2), 'utf8');
  return { reportPath: REPORT_PATH, verdict };
}

function summarize(dev, cycles, events) {
  const total = cycles.length;
  const online = cycles.filter(c => c.probe?.ok);
  const probeRtts = online.map(c => c.probe.rtt).filter(Number.isFinite);
  const mdnsHits = online.filter(c => c.probe.via === 'mdns').length;
  const rounds = online.filter(c => c.round);
  const logins = rounds.filter(c => c.round.login.ok);
  const loginMs = logins.map(c => c.round.login.ms);
  const menuAll = rounds.flatMap(c => c.round.menus);
  const menuOk = menuAll.filter(m => m.ok);
  const menuMs = menuOk.map(m => m.ms).filter(Number.isFinite);
  const perPage = {};
  for (const m of menuAll) {
    perPage[m.page] ??= { tries: 0, ok: 0, ms: [] };
    perPage[m.page].tries++;
    if (m.ok) { perPage[m.page].ok++; perPage[m.page].ms.push(m.ms); }
  }
  const heaps = cycles.map(c => c.memAfter?.freeHeap ?? c.mem?.freeHeap).filter(v => Number.isFinite(v));
  const maxAllocs = cycles.map(c => c.memAfter?.maxAlloc ?? c.mem?.maxAlloc).filter(v => Number.isFinite(v));
  const noMem = cycles.map(c => c.mem?.noMemoryFeatures).filter(v => Number.isFinite(v));
  const reboots = events.filter(e => e.type === 'reboot').length;
  const consoleErrs = rounds.flatMap(c => c.round.consoleErrors);
  const pageErrs = rounds.flatMap(c => c.round.pageErrors);
  const httpErrs = rounds.flatMap(c => c.round.httpErrors);
  const memory503 = cycles.reduce((n, c) => n + Number(Boolean(c.mem?.status503)) + Number(Boolean(c.memAfter?.status503)), 0);
  const webguard503 = rounds.reduce((n, c) => n + (c.round.webguard503 || 0), 0) + memory503;
  const domToasts = rounds.reduce((n, c) => n + (c.round.domErrorToasts || 0), 0);
  const first = cycles[0]?.t, last = cycles[total - 1]?.t;
  const firstMs = Date.parse(first || '');
  const lastMs = Date.parse(last || '');
  const windowMs = Number.isFinite(firstMs) && Number.isFinite(lastMs) ? Math.max(0, lastMs - firstMs) : 0;
  const skipped = cycles.filter(c => c.skipped);
  const cycleErrors = cycles.filter(c => c.error);
  const statusErrors = cycles.flatMap(c => [c.mem?.error, c.memAfter?.error]).filter(Boolean);
  const probeRetries = cycles.reduce((n, c) => n + Math.max(0, Number(c.probe?.attempts || 0) - 1), 0);
  const browserRetries = cycles.reduce((n, c) => n + Number(c.browserRetries || 0), 0);
  const navigationRetries = rounds.reduce((n, c) => n + Number(c.round.navigationRetries || 0), 0);
  const interactionRetries = rounds.reduce((n, c) => n + Number(c.round.interactionRetries || 0), 0);
  const ignoredSession401 = rounds.reduce((n, c) => n + Number(c.round.ignoredSession401 || 0), 0);
  const ignoredNavigationAborts = rounds.reduce((n, c) => n + Number(c.round.ignoredNavigationAborts || 0), 0);
  const menuSignatures = new Set(rounds
    .filter(c => c.round.login.ok)
    .map(c => [...(c.round.visibleMenus || [])].sort().join(',')));
  return {
    total, online, probeRtts, mdnsHits, rounds, logins, loginMs, perPage,
    menuAll, menuOk, menuMs, heaps, maxAllocs, noMem, reboots, events,
    consoleErrs, pageErrs, httpErrs, webguard503, domToasts, first, last, windowMs,
    skipped, cycleErrors, statusErrors, probeRetries, browserRetries,
    navigationRetries, interactionRetries, ignoredSession401,
    ignoredNavigationAborts, menuSignatures,
    onlineRate: pct(online.length, total),
    loginRate: pct(logins.length, online.length),
    menuRate: pct(menuOk.length, menuAll.length),
    lastHeap: heaps.length ? heaps[heaps.length - 1] : null,
    lastUptime: cycles[total - 1]?.memAfter?.uptime ?? cycles[total - 1]?.mem?.uptime ?? null,
    heapTrendDrop: trendDrop(heaps),
    maxAllocTrendDrop: trendDrop(maxAllocs),
  };
}

function evaluateAcceptance(summaries) {
  const reasons = [];
  const requiredWindowMs = OPTS.minDurationHours * 3_600_000;
  for (const { dev, summary: s } of summaries) {
    const p = dev.label;
    if (s.total === 0) {
      reasons.push(`${p}: 没有监测周期`);
      continue;
    }
    if (requiredWindowMs > 0 && s.windowMs < requiredWindowMs) {
      reasons.push(`${p}: 实际窗口 ${(s.windowMs / 3_600_000).toFixed(3)}h，小于要求 ${OPTS.minDurationHours}h`);
    }
    if (s.online.length !== s.total) reasons.push(`${p}: 在线周期 ${s.online.length}/${s.total}`);
    if (s.rounds.length !== s.total) reasons.push(`${p}: 浏览器完整覆盖 ${s.rounds.length}/${s.total}`);
    if (s.skipped.length) reasons.push(`${p}: ${s.skipped.length} 个周期因降载被跳过`);
    if (s.cycleErrors.length) reasons.push(`${p}: ${s.cycleErrors.length} 个周期发生看门狗或执行错误`);
    if (s.statusErrors.length) reasons.push(`${p}: system status 采样失败 ${s.statusErrors.length} 次`);
    if (s.probeRetries) reasons.push(`${p}: HTTP 探针发生 ${s.probeRetries} 次瞬时失败后重试`);
    if (s.browserRetries) reasons.push(`${p}: 浏览器登录发生 ${s.browserRetries} 次重试`);
    if (s.navigationRetries + s.interactionRetries > 0) {
      reasons.push(`${p}: 浏览器导航/交互发生 ${s.navigationRetries + s.interactionRetries} 次重试`);
    }
    if (s.logins.length !== s.rounds.length) reasons.push(`${p}: 登录通过 ${s.logins.length}/${s.rounds.length}`);
    if (!s.menuAll.length || s.menuOk.length !== s.menuAll.length) {
      reasons.push(`${p}: 菜单通过 ${s.menuOk.length}/${s.menuAll.length}`);
    }
    const missingCore = s.rounds.filter(c => c.round.login.ok).flatMap(c =>
      ['dashboard', 'device'].filter(menu => !(c.round.visibleMenus || []).includes(menu)));
    if (missingCore.length) reasons.push(`${p}: ${missingCore.length} 次缺少 dashboard/device 基本菜单`);
    if (s.menuSignatures.size > 1) reasons.push(`${p}: 可见菜单集合在监测期间发生变化`);
    if (s.reboots) reasons.push(`${p}: 检测到 ${s.reboots} 次重启`);
    if (s.webguard503) reasons.push(`${p}: 检测到 ${s.webguard503} 次 WebGuard/system status 503`);
    if (s.httpErrs.length) reasons.push(`${p}: HTTP 4xx/5xx 或请求失败 ${s.httpErrs.length} 次`);
    if (s.consoleErrs.length) reasons.push(`${p}: console error ${s.consoleErrs.length} 条`);
    if (s.pageErrs.length) reasons.push(`${p}: pageerror ${s.pageErrs.length} 条`);
    if (s.domToasts) reasons.push(`${p}: 页面错误提示 ${s.domToasts} 次`);
    if (s.probeRtts.length && percentile(s.probeRtts, 0.95) > ACCEPT_MAX_PROBE_P95_MS) {
      reasons.push(`${p}: 探针 p95 ${Math.round(percentile(s.probeRtts, 0.95))}ms 超过 ${ACCEPT_MAX_PROBE_P95_MS}ms`);
    }
    if (s.menuMs.length && percentile(s.menuMs, 0.95) > ACCEPT_MAX_MENU_P95_MS) {
      reasons.push(`${p}: 菜单响应 p95 ${Math.round(percentile(s.menuMs, 0.95))}ms 超过 ${ACCEPT_MAX_MENU_P95_MS}ms`);
    }
    if (s.heaps.length !== s.online.length) reasons.push(`${p}: 有效内部 DRAM 样本 ${s.heaps.length}/${s.online.length}`);
    if (s.heaps.length && Math.min(...s.heaps) < ACCEPT_MIN_HEAP) {
      reasons.push(`${p}: 内部 DRAM 最低 ${Math.min(...s.heaps)}B，小于 ${ACCEPT_MIN_HEAP}B`);
    }
    if (s.maxAllocs.length !== s.online.length) reasons.push(`${p}: 有效最大连续块样本 ${s.maxAllocs.length}/${s.online.length}`);
    if (s.maxAllocs.length && Math.min(...s.maxAllocs) < ACCEPT_MIN_MAX_ALLOC) {
      reasons.push(`${p}: 最大连续块最低 ${Math.min(...s.maxAllocs)}B，小于 ${ACCEPT_MIN_MAX_ALLOC}B`);
    }
    if (s.heapTrendDrop > ACCEPT_MAX_HEAP_DROP) {
      reasons.push(`${p}: 内部 DRAM 首尾窗口下降 ${Math.round(s.heapTrendDrop)}B，超过 ${ACCEPT_MAX_HEAP_DROP}B`);
    }
    if (s.maxAllocTrendDrop > ACCEPT_MAX_ALLOC_DROP) {
      reasons.push(`${p}: 最大连续块首尾窗口下降 ${Math.round(s.maxAllocTrendDrop)}B，超过 ${ACCEPT_MAX_ALLOC_DROP}B`);
    }
  }
  return {
    ok: reasons.length === 0,
    generatedAt: nowIso(),
    minDurationHours: OPTS.minDurationHours,
    devices: summaries.map(({ dev }) => dev.label),
    reasons,
  };
}

function fmtMs(ms) { return ms >= 1000 ? `${(ms / 1000).toFixed(1)}s` : `${Math.round(ms)}ms`; }
function topCounts(arr, n = 5) {
  const map = {};
  for (const s of arr) { const k = s.slice(0, 120); map[k] = (map[k] || 0) + 1; }
  return Object.entries(map).sort((a, b) => b[1] - a[1]).slice(0, n);
}

function renderDeviceSection(dev, s) {
  const L = [];
  L.push(`## ${dev.label}（${dev.model}）`);
  L.push('');
  if (s.total === 0) {
    L.push('暂无监测数据。');
    L.push('');
    return L.join('\n');
  }
  L.push(`监测窗口：${s.first} → ${s.last}（${(s.windowMs / 3_600_000).toFixed(3)}h）　|　完成周期：**${s.total}**　|　重启次数：**${s.reboots}**`);
  L.push('');

  // 1. 可用性与响应时间
  L.push('### 在线率与响应时间');
  L.push('');
  L.push(`- HTTP 探针在线率：**${s.onlineRate.toFixed(1)}%**（${s.online.length}/${s.total}），其中 mDNS 直达 ${s.mdnsHits} 次`);
  if (s.probeRtts.length) {
    L.push(`- 探针响应：avg ${fmtMs(s.probeRtts.reduce((a, b) => a + b, 0) / s.probeRtts.length)} / p50 ${fmtMs(percentile(s.probeRtts, 0.5))} / p95 ${fmtMs(percentile(s.probeRtts, 0.95))} / max ${fmtMs(Math.max(...s.probeRtts))}`);
  }
  if (s.loginMs.length) {
    L.push(`- 登录耗时：avg ${fmtMs(s.loginMs.reduce((a, b) => a + b, 0) / s.loginMs.length)} / max ${fmtMs(Math.max(...s.loginMs))}，通过率 **${s.loginRate.toFixed(1)}%**`);
  } else if (s.online.length > 0) {
    L.push('- 登录：**无一次成功**');
  }
  L.push('');

  // 2. 菜单覆盖
  L.push('### 功能模块（菜单页）通过率');
  L.push('');
  const pages = Object.keys(s.perPage);
  if (pages.length) {
    L.push('| 菜单页 | 次数 | 通过 | 通过率 | 平均耗时 | 最大耗时 |');
    L.push('|--------|-----:|-----:|-------:|---------:|---------:|');
    for (const p of pages.sort()) {
      const v = s.perPage[p];
      const avg = v.ms.length ? v.ms.reduce((a, b) => a + b, 0) / v.ms.length : 0;
      const max = v.ms.length ? Math.max(...v.ms) : 0;
      L.push(`| ${p} | ${v.tries} | ${v.ok} | ${pct(v.ok, v.tries).toFixed(0)}% | ${fmtMs(avg)} | ${fmtMs(max)} |`);
    }
    L.push('');
    L.push(`总体菜单通过率：**${s.menuRate.toFixed(1)}%**（${s.menuOk.length}/${s.menuAll.length}）`);
  } else {
    L.push('无菜单遍历数据（登录未成功或设备离线）。');
  }
  L.push('');

  // 3. 内存
  L.push('### 内存使用与风险');
  L.push('');
  if (s.heaps.length) {
    const min = Math.min(...s.heaps), max = Math.max(...s.heaps);
    const avg = s.heaps.reduce((a, b) => a + b, 0) / s.heaps.length;
    L.push(`- freeHeap：min **${(min / 1024).toFixed(1)}KB** / avg ${(avg / 1024).toFixed(1)}KB / 最新 ${(s.lastHeap / 1024).toFixed(1)}KB / max ${(max / 1024).toFixed(1)}KB`);
    const noMemLast = s.noMem.length ? s.noMem[s.noMem.length - 1] : null;
    if (noMemLast !== null) L.push(`- 因内存不足未加载的功能数（no-memory）：${noMemLast}`);
    if (s.maxAllocs.length) L.push(`- 最大连续块：最低 **${Math.min(...s.maxAllocs)}B** / 首尾窗口变化 ${Math.round(s.maxAllocTrendDrop)}B`);
    L.push(`- 内部 DRAM 首尾窗口变化：${Math.round(s.heapTrendDrop)}B（正数表示下降）`);
    if (s.lastUptime !== null) L.push(`- 当前 uptime：${(s.lastUptime / 3600000).toFixed(2)}h`);
  } else {
    L.push('无内存采样数据。');
  }
  L.push('');

  // 4. 错误日志
  L.push('### 错误日志');
  L.push('');
  L.push(`- WebGuard 503 拒绝：**${s.webguard503}** 次　|　页面错误提示(toast)：${s.domToasts} 次`);
  L.push(`- HTTP 4xx/5xx/失败请求：${s.httpErrs.length} 次　|　console error：${s.consoleErrs.length} 条　|　pageerror：${s.pageErrs.length} 条`);
  L.push(`- 探针重试：${s.probeRetries} 次　|　登录重试：${s.browserRetries} 次　|　导航/交互重试：${s.navigationRetries + s.interactionRetries} 次`);
  L.push(`- 已识别正常噪声：未登录会话探测 401 ${s.ignoredSession401} 次，页面切换主动取消 ${s.ignoredNavigationAborts} 次`);
  for (const [title, arr] of [['HTTP 错误 TOP', s.httpErrs], ['console error TOP', s.consoleErrs], ['pageerror TOP', s.pageErrs]]) {
    const tops = topCounts(arr);
    if (tops.length) {
      L.push(`- ${title}：`);
      for (const [msg, n] of tops) L.push(`  - \`${msg}\` ×${n}`);
    }
  }
  const rebootEvents = s.events.filter(e => e.type === 'reboot');
  if (rebootEvents.length) {
    L.push('- 重启事件：');
    for (const e of rebootEvents.slice(-5)) L.push(`  - ${e.t}（uptime ${e.uptimeWas} → ${e.uptimeNow}）`);
  }
  L.push('');

  // 5. 风险预警
  L.push('### 风险预警与评估');
  L.push('');
  const risks = [];
  if (s.onlineRate < 99.9) risks.push(`🔴 在线率 ${s.onlineRate.toFixed(1)}% < 100%，存在不可达周期`);
  if (s.reboots > 0) risks.push(`🔴 监测期内发生 ${s.reboots} 次重启`);
  if (s.heaps.length) {
    const min = Math.min(...s.heaps);
    if (min < RISK_GUARD_TRIGGER) risks.push(`🔴 freeHeap 最低 ${(min / 1024).toFixed(1)}KB 低于 WebGuard 触发线 20KB，API 会被 503 拒绝`);
    else if (min < RISK_GUARD_RECOVERY) risks.push(`🟠 freeHeap 最低 ${(min / 1024).toFixed(1)}KB 低于 WebGuard 恢复线 21KB，触发后难以自愈`);
    else if (min < RISK_WATCH) risks.push(`🟡 freeHeap 最低 ${(min / 1024).toFixed(1)}KB 处于观察线 30KB 以下，内存紧张`);
  }
  if (s.webguard503 > 0) risks.push(`🟠 出现 ${s.webguard503} 次 WebGuard 503 拒绝（内存压力实证）`);
  if (s.skipped.length > 0) risks.push(`🟠 ${s.skipped.length} 个周期被降载跳过，浏览器覆盖不完整`);
  if (s.cycleErrors.length + s.statusErrors.length > 0) risks.push(`🟠 周期/状态采样错误 ${s.cycleErrors.length + s.statusErrors.length} 次`);
  if (s.httpErrs.length > 0) risks.push(`🟠 存在 ${s.httpErrs.length} 次 HTTP 4xx/5xx 或请求失败`);
  if (s.probeRetries + s.browserRetries + s.navigationRetries + s.interactionRetries > 0) risks.push('🟠 存在瞬时失败后重试，未达到无错误长期验收标准');
  if (s.online.length && s.loginRate < 100) risks.push(`🟠 登录通过率 ${s.loginRate.toFixed(1)}%，存在登录失败`);
  if (s.menuAll.length && s.menuRate < 100) {
    const fails = Object.entries(s.perPage).filter(([, v]) => v.ok < v.tries).map(([p, v]) => `${p}(${v.ok}/${v.tries})`);
    risks.push(`🟠 菜单通过率 ${s.menuRate.toFixed(1)}%，失败页：${fails.join(', ')}`);
  }
  if (s.consoleErrs.length + s.pageErrs.length > 0) risks.push(`🟡 存在 ${s.consoleErrs.length + s.pageErrs.length} 条前端错误（详见错误日志）`);
  if (risks.length === 0) L.push('✅ 未发现风险项：在线率 100%、无重启、登录与菜单全通过、内存水位健康。');
  else L.push(...risks.map(r => `- ${r}`));
  L.push('');

  // 综合评级
  let grade = 'A（稳定）';
  if (s.onlineRate < 95 || s.reboots > 2 || s.loginRate < 80) grade = 'D（不稳定，需立即处理）';
  else if ((s.heaps.length > 0 && Math.min(...s.heaps) < ACCEPT_MIN_HEAP) || s.onlineRate < 99 || s.reboots > 0 || s.loginRate < 95 || s.menuRate < 95 || s.cycleErrors.length > 0) grade = 'C（有明显风险）';
  else if (s.onlineRate < 100 || s.menuRate < 100 || s.webguard503 > 0 || s.httpErrs.length > 0 || s.consoleErrs.length > 0 || s.pageErrs.length > 0 || s.skipped.length > 0 || s.probeRetries + s.browserRetries + s.navigationRetries + s.interactionRetries > 0) grade = 'B（基本稳定，存在间歇性问题）';
  L.push(`**综合评级：${grade}**`);
  L.push('');
  return L.join('\n');
}

// ─── 主流程 ───────────────────────────────────────────
async function main() {
  if (OPTS.reportOnly) {
    fs.mkdirSync(OUT_DIR, { recursive: true });
    const result = generateReport();
    console.log(`报告已生成: ${result.reportPath}`);
    return;
  }

  assertFreshOutput();
  fs.mkdirSync(OUT_DIR, { recursive: true });
  fs.writeFileSync(path.join(OUT_DIR, 'run-metadata.json'), JSON.stringify({
    startedAt: nowIso(),
    devices: DEVICES_ACTIVE.map(d => ({ label: d.label, model: d.model, host: d.host })),
    intervalSeconds: OPTS.interval,
    maxCycles: OPTS.maxCycles,
    minDurationHours: OPTS.minDurationHours,
    cycleTimeoutMs: OPTS.cycleTimeout,
  }, null, 2), 'utf8');

  log(`═══ 舰队长期稳定性监测启动 ═══ 设备=${DEVICES_ACTIVE.map(d => d.label).join(', ')} 间隔=${OPTS.interval}s 最大周期=${OPTS.maxCycles || '∞'}`);

  const browserHolder = { browser: await chromium.launch({ headless: true }) };
  const stopFlag = { value: false };
  let finished = false;

  const shutdown = async (sig) => {
    if (stopFlag.value) return;
    log(`收到 ${sig}，正在优雅退出（等待当前周期结束）...`);
    stopFlag.value = true;
    setTimeout(() => { log('退出超时，强制结束'); process.exit(1); }, 120_000).unref();
  };
  process.on('SIGINT', () => shutdown('SIGINT'));
  process.on('SIGTERM', () => shutdown('SIGTERM'));

  // 报告周期生成器
  const reportTimer = setInterval(() => {
    try { generateReport(); log('📄 定期报告已更新'); } catch (e) { log(`报告生成失败: ${e.message}`); }
  }, OPTS.interval * OPTS.reportEvery * 1000);

  // 各设备错峰启动（30s 间隔，避免同时冲击各自设备的 Web 服务）
  const workerResults = await Promise.allSettled(DEVICES_ACTIVE.map(async (dev, i) => {
    await sleep(i * 30_000);
    await monitorDevice(dev, browserHolder, stopFlag);
  }));
  const workerErrors = workerResults
    .map((result, i) => result.status === 'rejected' ? `${DEVICES_ACTIVE[i].label}: worker ${String(result.reason?.message || result.reason)}` : null)
    .filter(Boolean);
  finished = !stopFlag.value && workerErrors.length === 0;

  clearInterval(reportTimer);
  await browserHolder.browser.close().catch(() => {});
  const result = generateReport(workerErrors);
  log(`═══ 监测结束（${finished ? '完成' : '中断'}），验收=${result.verdict.ok ? 'PASS' : 'FAIL'}，最终报告: ${result.reportPath} ═══`);
  if (!finished || !result.verdict.ok) process.exitCode = 2;
}

main().catch(e => {
  console.error('监测器致命错误:', e);
  try { generateReport(); } catch {}
  process.exit(1);
});
