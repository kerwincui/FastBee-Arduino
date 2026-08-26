#!/usr/bin/env node
/**
 * FastBee 舰队长期稳定性监测器（Playwright 浏览器自动化）
 *
 * 对舰队 4 台设备持续执行真实用户视角的 Web 全链路验证：
 *   1. HTTP 探针（mDNS 域名优先，IP 兜底）→ 在线率 / 响应时间
 *   2. 浏览器打开主页 → 登录（admin/admin123）→ 验证 #app-container 显示
 *   3. 遍历侧边栏全部菜单页，验证每页内容非空、无 console/pageerror/HTTP 5xx
 *   4. 采样 /api/system/status（freeHeap/uptime/features）→ 内存趋势 + 重启检测
 *   5. 每周期追加 JSONL 记录；周期性生成 fleet-stability-report.md
 *
 * 用法:
 *   node fleet-stability-monitor.mjs                     # 无限循环，间隔 180s
 *   node fleet-stability-monitor.mjs --once              # 单轮冒烟
 *   node fleet-stability-monitor.mjs --interval 120 --report-every 8
 *   node fleet-stability-monitor.mjs --max-cycles 40
 *   node fleet-stability-monitor.mjs --devices fastbee.local,fastbee-4.local
 *   node fleet-stability-monitor.mjs --report-only       # 仅基于已有 JSONL 重新生成报告
 *
 * 数据输出: .pio/fleet-monitor/<device>/cycles.jsonl + events.jsonl
 * 报告输出: .pio/fleet-monitor/fleet-stability-report.md
 */

import { chromium } from 'playwright';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(__dirname, '..', '..');
const OUT_DIR = path.join(ROOT, '.pio', 'fleet-monitor');
const REPORT_PATH = path.join(OUT_DIR, 'fleet-stability-report.md');

// ─── 舰队定义 ─────────────────────────────────────────
const DEVICES = [
  // 2026-08-24：.34 设备当前 DHCP 地址 192.168.5.19（mDNS fastbee.local 为准，
  // IP 仅作兑底；历史地址 .34 保留）
  { label: 'fastbee.local',   host: 'fastbee.local',   ips: ['192.168.5.19', '192.168.5.34'], model: 'ESP32 经典 (lite)' },
  { label: 'fastbee-2.local', host: 'fastbee-2.local', ips: ['192.168.5.30'], model: 'ESP32-C3' },
  { label: 'fastbee-3.local', host: 'fastbee-3.local', ips: ['192.168.5.31'], model: 'ESP32-C6' },
  { label: 'fastbee-4.local', host: 'fastbee-4.local', ips: ['192.168.5.14'], model: 'ESP32-S3' },
];

const AUTH = { username: 'admin', password: 'admin123' };
// WebMemoryGuard 阈值（include/core/MemoryBudget.h）：free8<20480 触发 503，恢复需 >=24576
const RISK_GUARD_TRIGGER = 20480;
const RISK_GUARD_RECOVERY = 24576;
const RISK_WATCH = 30720; // 观察线：低于此值视为内存紧张
// 降载退避：freeHeap 低于此值或上轮有 503 时，本轮只探针不浏览，给设备恢复窗口
const LIGHT_CYCLE_HEAP = 45000;
// 碎片退避（2026-08-24 .34 取证）：freeHeap 尚余 45K+ 但 maxAlloc 已碎到
// 8.7KB 时浏览器轮直接把设备压进 CRITICAL→碎片重启。maxAlloc 低于此值
// （=GUARD_SEVERE_LARGEST_BLOCK）时同样跳过浏览器轮
const LIGHT_CYCLE_MAX_ALLOC = 12288;
const MENU_DELAY_NORMAL = 1500;
const MENU_DELAY_GENTLE = 3000;

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
  }
  if (opts.once) opts.maxCycles = 1;
  return opts;
}
const OPTS = parseArgs();
const DEVICES_ACTIVE = OPTS.devices
  ? DEVICES.filter(d => OPTS.devices.includes(d.label))
  : DEVICES;

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
async function timedFetch(url, timeoutMs = 10_000) {
  const t0 = Date.now();
  try {
    const resp = await fetch(url, { signal: AbortSignal.timeout(timeoutMs) });
    const rtt = Date.now() - t0;
    return { ok: resp.ok, status: resp.status, rtt, text: await resp.text().catch(() => '') };
  } catch (e) {
    return { ok: false, status: 0, rtt: Date.now() - t0, error: String(e?.cause?.code || e?.message || e) };
  }
}

// ─── HTTP 探针：mDNS 优先，IP 兜底，两轮重试 ──────────
// 经典 ESP32 低堆时偶发连接复位（000），mDNS 冷缓存首查可能失败，
// 因此做两轮：首轮全候选，次轮重试全部候选再判定离线
async function probeDevice(dev) {
  const candidates = [{ via: 'mdns', base: `http://${dev.host}` },
                      ...dev.ips.map(ip => ({ via: 'ip', base: `http://${ip}` }))];
  for (let round = 0; round < 2; round++) {
    for (const c of candidates) {
      const r = await timedFetch(`${c.base}/api/health`, 10_000);
      if (r.ok) return { ok: true, base: c.base, via: c.via, rtt: r.rtt, status: r.status };
    }
    if (round === 0) await sleep(3_000);
  }
  return { ok: false, base: `http://${dev.host}`, via: 'none', rtt: 0, error: 'unreachable' };
}

// ─── 内存采样（/api/system/status 公开端点）───────────
async function sampleMemory(base) {
  const r = await timedFetch(`${base}/api/system/status`, 10_000);
  if (!r.ok) return null;
  try {
    const data = JSON.parse(r.text).data || {};
    const features = data.features || {};
    const noMemory = Object.values(features).filter(v => v === 'no-memory').length;
    return {
      freeHeap: Number(data.freeHeap || 0),
      maxAlloc: Number(data.memory?.heapMaxAlloc || 0),
      uptime: Number(data.uptime || 0),
      rssi: data.rssi ?? null,
      noMemoryFeatures: noMemory,
      rtt: r.rtt,
      status503: r.status === 503,
    };
  } catch { return null; }
}

// ─── 浏览器轮：登录 + 菜单遍历 + 错误采集 ─────────────
async function browserRound(dev, context, base, menuDelay = MENU_DELAY_NORMAL) {
  const round = {
    login: { ok: false, ms: 0, error: null },
    menus: [],
    consoleErrors: [],
    pageErrors: [],
    httpErrors: [],       // status >= 500 或 requestfailed
    webguard503: 0,       // WebMemoryGuard 拒绝计数
    domErrorToasts: 0,
  };
  const page = await context.newPage();
  page.setDefaultTimeout(30_000);
  page.on('console', m => {
    if (m.type() === 'error') round.consoleErrors.push(m.text().slice(0, 300));
  });
  page.on('pageerror', e => round.pageErrors.push(String(e).slice(0, 300)));
  page.on('requestfailed', req => {
    const url = req.url();
    if (!url.includes('favicon')) round.httpErrors.push(`failed ${req.method()} ${url.slice(0, 120)} ${req.failure()?.errorText || ''}`);
  });
  page.on('response', resp => {
    if (resp.status() === 503) round.webguard503++;
    else if (resp.status() >= 500) round.httpErrors.push(`${resp.status()} ${resp.request().method()} ${resp.url().slice(0, 120)}`);
  });
  page.on('dialog', d => d.accept().catch(() => {}));

  const t0 = Date.now();
  try {
    // 1) 打开首页（ESP32 单连接串行传输，高负载时首屏可达 30~60s，带重试放宽到 90s）
    for (let gotoRetry = 0; ; gotoRetry++) {
      try {
        await page.goto(`${base}/`, { waitUntil: 'domcontentloaded', timeout: 90_000 });
        break;
      } catch (gotoErr) {
        if (gotoRetry >= 1) throw gotoErr;
        await sleep(5_000);
      }
    }
    // 2) 判定登录态：登录页可见 → 需要登录
    const needLogin = await page.evaluate(() => {
      const lp = document.querySelector('#login-page');
      const app = document.querySelector('#app-container');
      if (lp && lp.style.display !== 'none') return true;
      if (app && !app.classList.contains('fb-hidden')) return false;
      return true; // 无法判定按需要登录处理
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

    // 自适应等待：modals 片段 / 首屏数据
    await page.waitForFunction(() => document.querySelector('.modal') !== null, { timeout: 10_000 }).catch(() => {});
    await sleep(1500);

    // 3) 枚举侧边栏可见菜单（前端按运行时能力位隐藏不支持项，display:none 不计入覆盖）
    const menuPages = await page.$$eval('.menu-item[data-page]', els =>
      els.filter(e => getComputedStyle(e).display !== 'none')
         .map(e => e.getAttribute('data-page')).filter(Boolean));

    for (const p of menuPages) {
      const m = { page: p, ok: false, ms: 0, error: null };
      const t1 = Date.now();
      try {
        // 侧边栏溢出视口的菜单项先滚动入视口，再点击（C6 实测 rule-script/data 在折叠区）
        await page.locator(`.menu-item[data-page="${p}"]`).scrollIntoViewIfNeeded({ timeout: 5_000 }).catch(() => {});
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
      await sleep(menuDelay); // 嵌入式设备串行处理，给设备喘息
    }

    // 4) DOM 错误提示扫描（notification/toast 中的错误项）
    round.domErrorToasts = await page.evaluate(() => {
      const sels = '.notification-error, .toast-error, .message-error, .alert-error';
      return Array.from(document.querySelectorAll(sels))
        .filter(el => el.offsetParent !== null).length;
    }).catch(() => 0);

    // 5) 回到 dashboard 收尾
    await page.click('.menu-item[data-page="dashboard"]', { timeout: 10_000 }).catch(() => {});
  } catch (e) {
    round.login.error = String(e?.message || e).split('\n')[0].slice(0, 300);
  } finally {
    await page.close().catch(() => {});
  }
  // 截断错误列表防止 JSONL 膨胀
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
  let lastUptime = null;
  let wasOnline = null;
  let consecutiveOffline = 0;
  let lastWebguard503 = 0; // 上轮 503 计数，驱动本轮降载

  const ensureContext = async () => {
    if (context && !browserHolder.browser.isConnected()) context = null;
    if (!context) {
      context = await browserHolder.browser.newContext({
        viewport: { width: 1366, height: 800 },
        extraHTTPHeaders: { 'Accept-Language': 'zh-CN' },
      });
    }
    return context;
  };
  const dropContext = async () => {
    if (context) { await context.close().catch(() => {}); context = null; }
  };

  while (!stopFlag.value) {
    cycle++;
    const cycleStart = Date.now();
    const rec = { t: nowIso(), cycle, device: dev.label, probe: null, round: null, mem: null, reboot: false, error: null };

    const runOnce = (async () => {
      // 1) HTTP 探针
      const probe = await probeDevice(dev);
      rec.probe = probe;
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
      if (rec.mem && lastUptime !== null && rec.mem.uptime > 0 && rec.mem.uptime < lastUptime - 5000) {
        rec.reboot = true;
        appendJsonl(eventsFile, { t: nowIso(), type: 'reboot', uptimeWas: lastUptime, uptimeNow: rec.mem.uptime });
        log(`${dev.label} #${cycle} ⚠ 检测到重启 (uptime ${lastUptime}→${rec.mem.uptime})`);
        await dropContext(); // session 失效
      }
      if (rec.mem) lastUptime = rec.mem.uptime;

      // 3) 浏览器轮（降载退避：低堆/碎片/上轮 503 时只探针不浏览，给设备恢复窗口）
      const heapNow = rec.mem?.freeHeap ?? Infinity;
      const maxAllocNow = rec.mem?.maxAlloc || Infinity;
      const gentle = heapNow < LIGHT_CYCLE_HEAP || maxAllocNow < LIGHT_CYCLE_MAX_ALLOC || lastWebguard503 > 0;
      if (heapNow < LIGHT_CYCLE_HEAP) {
        rec.skipped = 'low-heap';
        log(`${dev.label} #${cycle} 降载：heap=${heapNow} < ${LIGHT_CYCLE_HEAP}，本轮跳过浏览器轮`);
      } else if (maxAllocNow < LIGHT_CYCLE_MAX_ALLOC) {
        rec.skipped = 'fragmented';
        log(`${dev.label} #${cycle} 降载：maxAlloc=${maxAllocNow} < ${LIGHT_CYCLE_MAX_ALLOC}（碎片化），本轮跳过浏览器轮`);
      } else if (lastWebguard503 > 0) {
        rec.skipped = 'cooldown-503';
        log(`${dev.label} #${cycle} 降载：上轮 ${lastWebguard503} 次 503，本轮冷却跳过浏览器轮`);
      } else {
        for (let attempt = 0; attempt < 2; attempt++) {
          try {
            const ctx = await ensureContext();
            rec.round = await browserRound(dev, ctx, probe.base, gentle ? MENU_DELAY_GENTLE : MENU_DELAY_NORMAL);
            if (rec.round.login.ok) break;
            if (attempt === 0) {
              log(`${dev.label} #${cycle} 登录失败，休眠 10s 后重试: ${rec.round.login.error}`);
              await dropContext();
              await sleep(10_000);
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
      const memAfter = await sampleMemory(probe.base);
      if (memAfter) rec.memAfter = memAfter;
    })();

    // 看门狗：单周期超时则放弃并重建上下文
    await Promise.race([runOnce, (async () => {
      await sleep(OPTS.cycleTimeout);
      throw new Error('cycle timeout');
    })()]).catch(async (e) => {
      rec.error = rec.error || `watchdog: ${e.message}`;
      await dropContext();
      log(`${dev.label} #${cycle} 周期超时/异常: ${rec.error}`);
    });

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
function generateReport() {
  const t = nowIso();
  const L = []; // 行缓冲
  L.push('# FastBee 舰队长期稳定性测试报告');
  L.push('');
  L.push(`> 生成时间：${t}　|　监测方式：Playwright 无头浏览器（真实登录 + 菜单遍历）+ HTTP 探针`);
  L.push('');

  // 汇总表
  L.push('## 舰队总览');
  L.push('');
  L.push('| 设备 | 型号 | 周期数 | 在线率 | 登录通过率 | 菜单通过率 | 重启 | 503(WebGuard) | 最新 heap |');
  L.push('|------|------|-------:|-------:|-----------:|-----------:|-----:|--------------:|----------:|');

  const sections = [];
  for (const dev of DEVICES_ACTIVE) {
    const dir = path.join(OUT_DIR, slug(dev.label));
    const cycles = readJsonl(path.join(dir, 'cycles.jsonl'));
    const events = readJsonl(path.join(dir, 'events.jsonl'));
    const s = summarize(dev, cycles, events);
    L.push(`| ${dev.label} | ${dev.model} | ${s.total} | ${s.onlineRate.toFixed(1)}% | ${s.loginRate.toFixed(1)}% | ${s.menuRate.toFixed(1)}% | ${s.reboots} | ${s.webguard503} | ${s.lastHeap ?? '-'} |`);
    sections.push(renderDeviceSection(dev, s));
  }
  L.push('');
  L.push(...sections);
  L.push('---');
  L.push('');
  L.push('## 稳定性评估方法');
  L.push('');
  L.push('- **在线率** = HTTP 探针成功周期 / 总周期（mDNS 域名优先，IP 兜底）');
  L.push('- **登录通过率** = 浏览器成功登录并显示 #app-container 的周期 / 在线周期');
  L.push('- **菜单通过率** = 内容非空渲染成功的菜单次数 / 菜单点击总次数');
  L.push(`- **内存风险线**：freeHeap < ${RISK_GUARD_TRIGGER}B 触发 WebGuard 503；< ${RISK_GUARD_RECOVERY}B 无法恢复；< ${RISK_WATCH}B 为观察线`);
  L.push('- **重启检测**：uptime 回退即记一次重启事件');
  L.push('');
  fs.writeFileSync(REPORT_PATH, L.join('\n'), 'utf8');
  return REPORT_PATH;
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
  const perPage = {};
  for (const m of menuAll) {
    perPage[m.page] ??= { tries: 0, ok: 0, ms: [] };
    perPage[m.page].tries++;
    if (m.ok) { perPage[m.page].ok++; perPage[m.page].ms.push(m.ms); }
  }
  const heaps = cycles.map(c => c.memAfter?.freeHeap ?? c.mem?.freeHeap).filter(v => Number.isFinite(v));
  const noMem = cycles.map(c => c.mem?.noMemoryFeatures).filter(v => Number.isFinite(v));
  const reboots = events.filter(e => e.type === 'reboot').length;
  const consoleErrs = rounds.flatMap(c => c.round.consoleErrors);
  const pageErrs = rounds.flatMap(c => c.round.pageErrors);
  const httpErrs = rounds.flatMap(c => c.round.httpErrors);
  const webguard503 = rounds.reduce((n, c) => n + (c.round.webguard503 || 0), 0);
  const domToasts = rounds.reduce((n, c) => n + (c.round.domErrorToasts || 0), 0);
  const first = cycles[0]?.t, last = cycles[total - 1]?.t;
  return {
    total, online, probeRtts, mdnsHits, rounds, logins, loginMs, perPage,
    menuAll, menuOk, heaps, noMem, reboots, events,
    consoleErrs, pageErrs, httpErrs, webguard503, domToasts, first, last,
    onlineRate: pct(online.length, total),
    loginRate: pct(logins.length, online.length),
    menuRate: pct(menuOk.length, menuAll.length),
    lastHeap: heaps.length ? heaps[heaps.length - 1] : null,
    lastUptime: cycles[total - 1]?.mem?.uptime ?? null,
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
  L.push(`监测窗口：${s.first} → ${s.last}　|　完成周期：**${s.total}**　|　重启次数：**${s.reboots}**`);
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
    if (s.lastUptime !== null) L.push(`- 当前 uptime：${(s.lastUptime / 3600000).toFixed(2)}h`);
  } else {
    L.push('无内存采样数据。');
  }
  L.push('');

  // 4. 错误日志
  L.push('### 错误日志');
  L.push('');
  L.push(`- WebGuard 503 拒绝：**${s.webguard503}** 次　|　页面错误提示(toast)：${s.domToasts} 次`);
  L.push(`- HTTP 5xx/失败请求：${s.httpErrs.length} 次　|　console error：${s.consoleErrs.length} 条　|　pageerror：${s.pageErrs.length} 条`);
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
    else if (min < RISK_GUARD_RECOVERY) risks.push(`🟠 freeHeap 最低 ${(min / 1024).toFixed(1)}KB 低于 WebGuard 恢复线 24KB，触发后难以自愈`);
    else if (min < RISK_WATCH) risks.push(`🟡 freeHeap 最低 ${(min / 1024).toFixed(1)}KB 处于观察线 30KB 以下，内存紧张`);
  }
  if (s.webguard503 > 0) risks.push(`🟠 出现 ${s.webguard503} 次 WebGuard 503 拒绝（内存压力实证）`);
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
  else if (s.onlineRate < 99 || s.reboots > 0 || s.loginRate < 95 || s.menuRate < 95) grade = 'C（有明显风险）';
  else if (s.onlineRate < 100 || s.menuRate < 100 || s.webguard503 > 0) grade = 'B（基本稳定，存在间歇性问题）';
  L.push(`**综合评级：${grade}**`);
  L.push('');
  return L.join('\n');
}

// ─── 主流程 ───────────────────────────────────────────
async function main() {
  fs.mkdirSync(OUT_DIR, { recursive: true });

  if (OPTS.reportOnly) {
    const p = generateReport();
    console.log(`报告已生成: ${p}`);
    return;
  }

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
  await Promise.allSettled(DEVICES_ACTIVE.map(async (dev, i) => {
    await sleep(i * 30_000);
    await monitorDevice(dev, browserHolder, stopFlag);
  }));
  finished = true;

  clearInterval(reportTimer);
  await browserHolder.browser.close().catch(() => {});
  const p = generateReport();
  log(`═══ 监测结束（${finished ? '完成' : '中断'}），最终报告: ${p} ═══`);
}

main().catch(e => {
  console.error('监测器致命错误:', e);
  try { generateReport(); } catch {}
  process.exit(1);
});
