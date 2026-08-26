// FastBee E2E shared helpers
// 收敛弱信号/低内存设备反复出现的失败模式（2026-08-21 实测总结）：
//  1. goto 超时           -> openDevicePage 带 2 次重试（弱链路首屏可能 >20s）
//  2. 登录卡「页面初始化中」-> ensureBootReady 先等 window.__fastbeeBootReady
//  3. chunk 加载失败       -> 重载/重点击重试（SPA 页面按需注入 chunk 可能截断）
//  4. API 503/超时         -> apiWithRetry（低内存设备忙碌时瞬时 5xx）
//  5. 系统代理污染          -> 由 playwright.config.js 加载时统一清理
//  6. reload 时页面 detach  -> loginRobust 退化为整页 goto 重附（2026-08-22 实测 .34 间歇窗口）
//  7. 登录失败根因不明      -> loginRobust 抓取页面通知文本诊断：
//     「用户名或密码错误」= users.json 凭据漂移（需重刷文件系统），其余多为弱链路超时
//  8. 硬编码 IP 失效        -> resolveDevices()：DEVICES env 优先，缺省同步自动发现子网
//     （DHCP 漂移免疫；2026-08-22 实测旧兜底 .24 失效导致空跑）

const { execSync } = require('child_process');
const path = require('path');

const CREDENTIALS = { username: 'admin', password: 'admin123' };

/**
 * 设备列表解析：DEVICES env 优先；缺省调 discover-devices.js --ips 同步自动发现。
 * 发现失败返回空数组（playwright 报 no tests found，优于静默测试错误目标）。
 * @returns {string[]} IP 列表
 */
function resolveDevices() {
  if (process.env.DEVICES) {
    return process.env.DEVICES.split(',').map(s => s.trim()).filter(Boolean);
  }
  try {
    const script = path.join(__dirname, 'discover-devices.js');
    const out = execSync(`node "${script}" --ips`, {
      timeout: 90000,
      stdio: ['ignore', 'pipe', 'ignore'],
    }).toString().trim();
    const ips = out ? out.split(',').filter(Boolean) : [];
    // 回写 DEVICES：Playwright worker 是独立进程会重新 require 本文件，
    // 若不回写，设备瞬时离线会让主/worker 两次扫描结果不一致，
    // 导致 "Test not found in the worker process"（2026-08-22 实测 .34 窗口漂移）
    if (ips.length) {
      process.env.DEVICES = ips.join(',');
    }
    console.log(`[discover] auto-discovered fleet: ${ips.join(', ') || '(none)'}`);
    return ips;
  } catch {
    console.log('[discover] auto-discovery failed, no devices resolved');
    return [];
  }
}

/** goto + 等待登录表单，弱链路失败自动重试 2 次 */
async function openDevicePage(page, baseUrl) {
  let response = null;
  for (let attempt = 1; attempt <= 3; attempt++) {
    try {
      response = await page.goto(baseUrl, {
        waitUntil: 'domcontentloaded',
        timeout: attempt === 1 ? 30000 : 45000,
      });
      break;
    } catch (e) {
      if (attempt === 3) throw e;
      console.log(`  goto attempt ${attempt}/3 failed (${e.message.split('\n')[0]}), retrying...`);
      await page.waitForTimeout(3000);
    }
  }
  if (!response || !response.ok()) {
    throw new Error(`index page not OK: ${response ? response.status() : 'no response'}`);
  }
  await page.waitForSelector('#login-form', { timeout: 20000 });
}

/**
 * 等待 SPA boot chunk 就绪（window.__fastbeeBootReady）。
 * 未就绪就点登录会进入「页面初始化中...」15s 等待路径，弱链路上极易超时；
 * 先在这里主动等 boot（必要时触发 __fastbeeStartChunks），登录一步到位。
 */
async function ensureBootReady(page) {
  try {
    await page.waitForFunction(() => window.__fastbeeBootReady === true, null, { timeout: 35000 });
    return;
  } catch (e) {
    // boot chunk 可能尚未启动加载，主动触发后再等一轮
    await page.evaluate(() => {
      if (typeof window.__fastbeeStartChunks === 'function') window.__fastbeeStartChunks();
    }).catch(() => {});
    await page.waitForFunction(() => window.__fastbeeBootReady === true, null, { timeout: 30000 });
  }
}

/**
 * 健壮登录：等 boot 就绪 -> 填表 -> 提交 -> 等菜单。
 * 失败（登录响应在弱链路上可能丢包）自动重载重试，最多 3 轮；
 * reload 本身在间歇链路上可能超时/页面 detach，退化为整页 goto。
 * @param {import('@playwright/test').Page} page
 * @param {string} [baseUrl] 可选，提供后退化 goto 用；缺省取 page.url()
 */
async function loginRobust(page, baseUrl) {
  const fallbackUrl = baseUrl || page.url();
  let lastError = null;
  for (let round = 1; round <= 3; round++) {
    try {
      if (round > 1) {
        console.log(`  login round ${round}/3: reloading page...`);
        try {
          await page.reload({ waitUntil: 'domcontentloaded', timeout: 45000 });
        } catch (reloadErr) {
          console.log(`  reload failed (${reloadErr.message.split('\n')[0]}), fallback to goto...`);
          await page.goto(fallbackUrl, { waitUntil: 'domcontentloaded', timeout: 45000 });
        }
        await page.waitForSelector('#login-form', { timeout: 20000 });
      }
      await ensureBootReady(page);
      await page.fill('#username', CREDENTIALS.username);
      await page.fill('#password', CREDENTIALS.password);
      await page.click('#login-form button[type="submit"]');
      await page.waitForSelector('.menu-item', { timeout: 25000 });
      return;
    } catch (e) {
      lastError = e;
      console.log(`  login round ${round}/3 failed: ${e.message.split('\n')[0]}`);
      // 诊断：抓页面通知文本区分凭据漂移与弱链路（2026-08-22 实测 .34 历史残留 users.json）
      const notice = await page.evaluate(() => {
        const el = document.querySelector('#notification-container');
        return el ? el.textContent.replace(/\s+/g, ' ').trim().slice(0, 120) : '';
      }).catch(() => '');
      if (notice) {
        console.log(`  page notice: ${notice}`);
        if (/用户名或密码错误|Invalid username or password/i.test(notice)) {
          console.log('  !! 凭据不一致：设备 LittleFS 的 users.json 与仓库默认不符，需 uploadfs 重刷文件系统重置');
        }
      }
    }
  }
  throw lastError;
}

/** SPA 菜单切页 + active 断言，chunk 注入失败自动重点击一次 */
async function openSpaPage(page, name) {
  await page.click(`.menu-item[data-page="${name}"]`);
  const activePage = page.locator(`#${name}-page.active`).first();
  try {
    await activePage.waitFor({ state: 'visible', timeout: 20000 });
  } catch (e) {
    console.log(`  ${name} page first load failed, retrying once...`);
    await page.waitForTimeout(2000);
    await page.click(`.menu-item[data-page="${name}"]`);
    await activePage.waitFor({ state: 'visible', timeout: 25000 });
  }
}

/** API GET 重试（低内存设备忙碌时可能 5xx 或超时） */
async function apiWithRetry(page, request, url, attempts = 3, timeoutMs = 10000) {
  let res = null;
  for (let attempt = 1; attempt <= attempts; attempt++) {
    try {
      res = await request.get(url, { timeout: timeoutMs });
      if (res.status() < 500) return res;
    } catch (e) {
      res = null;
    }
    console.log(`  API ${url} attempt ${attempt}/${attempts} not ready, waiting 5s...`);
    await page.waitForTimeout(5000);
  }
  return res;
}

module.exports = { CREDENTIALS, openDevicePage, ensureBootReady, loginRobust, openSpaPage, apiWithRetry, resolveDevices };
