/**
 * FastBee E2E helpers 八大容错模式守护测试（防同类失效复发）
 *
 * 背景：fastbee-helpers.js 的容错逻辑全部来自真实故障修复（2026-08-21/22 实测），
 * 若后续重构悄悄破坏某条容错路径（如去掉重试、漏掉 reload→goto 退化），
 * 弱链路设备上会以「偶发失败」形式复发且难以定位。本 spec 用 mock Page 对
 * 每条容错路径做纯逻辑回归，不依赖真机/浏览器，任何环境可随时跑。
 *
 * 覆盖（对应 helpers 头注释 1-8）：
 *   1. goto 超时重试          -> openDevicePage：前两次超时、第三次成功则通过；三次全败才抛
 *   2. 登录卡初始化           -> ensureBootReady：首等超时触发 __fastbeeStartChunks 再等一轮
 *   3. chunk 截断             -> openSpaPage：active 首等失败后自动重点击一次
 *   4. API 503/超时           -> apiWithRetry：5xx/异常重试，<500 立即返回
 *   5. 系统代理污染           -> 见 discovery-regression.spec.js（config 加载清理断言）
 *   6. reload detach 退化     -> loginRobust：reload 抛错时退化整页 goto
 *   7. 登录失败通知诊断       -> loginRobust：抓「用户名或密码错误」并提示重刷文件系统
 *   8. 硬编码 IP 失效         -> 见 discovery-regression.spec.js（自动发现/DEVICES 回写断言）
 *
 * 用法：npx playwright test test/e2e/helpers-fault-tolerance.spec.js --project=chromium
 */

const { test, expect } = require('@playwright/test');
const {
  openDevicePage, ensureBootReady, loginRobust, openSpaPage, apiWithRetry,
} = require('./helpers/fastbee-helpers');

/**
 * 构造 mock Page：按脚本化行为模拟 Playwright Page 的关键方法。
 * @param {object} [opts]
 * @param {number} [opts.gotoFailures=0] goto 连续失败次数（之后成功）
 * @param {number} [opts.gotoBadStatus]  若设置，goto 返回该非 2xx 状态（不抛错）
 * @param {number} [opts.bootWaitFailures=0] waitForFunction(boot) 连续失败次数
 * @param {number} [opts.loginFailRounds=0] 登录轮 waitForSelector('.menu-item') 失败轮数
 * @param {boolean} [opts.reloadFails=false] reload 抛错（模拟 detach/超时）
 * @param {string} [opts.notice=''] 页面通知文本（凭据漂移诊断用）
 * @param {number} [opts.spaPageFailures=0] openSpaPage active 首等失败次数
 */
function mockPage(opts = {}) {
  const o = {
    gotoFailures: 0, gotoBadStatus: undefined, bootWaitFailures: 0,
    loginFailRounds: 0, reloadFails: false, notice: '', spaPageFailures: 0,
    ...opts,
  };
  const calls = { goto: 0, startChunks: 0, reload: 0, fill: 0, click: 0, evaluate: 0 };
  let menuWaits = 0;
  let bootWaits = 0;
  let spaWaits = 0;

  const page = {
    calls,
    url: () => 'http://mock-device.local/',
    waitForTimeout: async () => {},
    waitForSelector: async sel => {
      if (sel === '.menu-item') {
        menuWaits += 1;
        if (menuWaits <= o.loginFailRounds) throw new Error('mock: menu timeout');
      }
      return {};
    },
    goto: async () => {
      calls.goto += 1;
      if (calls.goto <= o.gotoFailures) throw new Error('mock: goto timeout');
      return {
        ok: () => !o.gotoBadStatus || o.gotoBadStatus < 400,
        status: () => o.gotoBadStatus || 200,
      };
    },
    reload: async () => {
      calls.reload += 1;
      if (o.reloadFails) throw new Error('mock: reload detached');
      return {};
    },
    waitForFunction: async () => {
      bootWaits += 1;
      if (bootWaits <= o.bootWaitFailures) throw new Error('mock: boot not ready');
    },
    evaluate: async fn => {
      calls.evaluate += 1;
      // 与 ensureBootReady 的触发脚本保持一致：识别 __fastbeeStartChunks 调用
      const src = String(fn);
      if (src.includes('__fastbeeStartChunks')) calls.startChunks += 1;
      if (src.includes('notification-container')) return o.notice;
      return undefined;
    },
    fill: async () => { calls.fill += 1; },
    click: async () => { calls.click += 1; },
    locator: () => ({
      first: () => ({
        waitFor: async () => {
          spaWaits += 1;
          if (spaWaits <= o.spaPageFailures) throw new Error('mock: chunk truncated');
        },
      }),
    }),
  };
  return page;
}

const pageStub = { waitForTimeout: async () => {} };

test.describe('helpers 八大容错模式守护', () => {
  // —— 模式 1：goto 超时重试 ——
  test('openDevicePage: goto 前两次超时第三次成功则通过', async () => {
    const page = mockPage({ gotoFailures: 2 });
    await openDevicePage(page, 'http://mock/');
    expect(page.calls.goto).toBe(3);
  });

  test('openDevicePage: goto 三次全败才抛错（重试预算未被削减）', async () => {
    const page = mockPage({ gotoFailures: 3 });
    await expect(openDevicePage(page, 'http://mock/')).rejects.toThrow(/goto timeout/);
    expect(page.calls.goto).toBe(3);
  });

  test('openDevicePage: 非 2xx 响应明确报错（弱链路上偶发 5xx 首页）', async () => {
    const page = mockPage({ gotoBadStatus: 503 });
    await expect(openDevicePage(page, 'http://mock/')).rejects.toThrow(/not OK/);
  });

  // —— 模式 2：boot 就绪等待 + chunk 主动触发 ——
  test('ensureBootReady: 首等超时后触发 __fastbeeStartChunks 再等一轮', async () => {
    const page = mockPage({ bootWaitFailures: 1 });
    await ensureBootReady(page);
    expect(page.calls.startChunks).toBe(1);
  });

  test('ensureBootReady: 两轮都不就绪才抛错', async () => {
    const page = mockPage({ bootWaitFailures: 99 });
    await expect(ensureBootReady(page)).rejects.toThrow(/boot not ready/);
    expect(page.calls.startChunks).toBe(1);
  });

  // —— 模式 3：chunk 截断重点击 ——
  test('openSpaPage: active 首等失败自动重点击一次后成功', async () => {
    const page = mockPage({ spaPageFailures: 1 });
    await openSpaPage(page, 'network');
    // 首次点击 + 重试点击 = 2
    expect(page.calls.click).toBe(2);
  });

  test('openSpaPage: 重点击后仍失败才抛错', async () => {
    const page = mockPage({ spaPageFailures: 99 });
    await expect(openSpaPage(page, 'network')).rejects.toThrow(/chunk truncated/);
    expect(page.calls.click).toBe(2);
  });

  // —— 模式 4：API 5xx/超时重试 ——
  test('apiWithRetry: 503 两次后 200，返回成功响应', async () => {
    let n = 0;
    const request = {
      get: async () => {
        n += 1;
        const status = n < 3 ? 503 : 200;
        return { status: () => status };
      },
    };
    const res = await apiWithRetry(pageStub, request, '/api/x');
    expect(res.status()).toBe(200);
    expect(n).toBe(3);
  });

  test('apiWithRetry: 401 等 4xx 不重试（业务错误立即返回）', async () => {
    let n = 0;
    const request = { get: async () => { n += 1; return { status: () => 401 }; } };
    const res = await apiWithRetry(pageStub, request, '/api/x');
    expect(res.status()).toBe(401);
    expect(n).toBe(1);
  });

  // —— 模式 6：reload detach 退化为 goto ——
  test('loginRobust: reload 失败退化为整页 goto，登录最终成功', async () => {
    const page = mockPage({ loginFailRounds: 1, reloadFails: true });
    await loginRobust(page, 'http://mock-device.local/');
    expect(page.calls.reload).toBe(1);
    // 首轮 goto 不算，退化 goto 至少 1 次
    expect(page.calls.goto).toBeGreaterThanOrEqual(1);
  });

  // —— 模式 7：登录失败通知诊断（凭据漂移识别）——
  test('loginRobust: 凭据漂移通知被抓取，三轮耗尽后抛错', async () => {
    const page = mockPage({ loginFailRounds: 99, notice: '用户名或密码错误' });
    await expect(loginRobust(page, 'http://mock/')).rejects.toThrow(/menu timeout/);
    // 每轮失败都会抓一次通知文本做诊断
    expect(page.calls.evaluate).toBeGreaterThanOrEqual(3);
    // 三轮都执行了填表（重试预算未被削减）
    expect(page.calls.fill).toBe(6);
  });

  test('loginRobust: 正常路径一次登录成功（无多余重试）', async () => {
    const page = mockPage({});
    await loginRobust(page, 'http://mock/');
    expect(page.calls.fill).toBe(2); // username + password 各一次
    expect(page.calls.reload).toBe(0);
  });
});
