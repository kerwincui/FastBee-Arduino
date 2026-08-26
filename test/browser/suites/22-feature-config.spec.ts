import { test, expect, env, hasMenuPage, waitForHealth } from '../fixtures/base.fixture';
import type { Page } from '@playwright/test';

/**
 * Suite-22: 功能配置页（features.html / /api/features/*）
 *
 * 覆盖运行时功能开关页面的 UI 行为：
 * - 列表渲染与 API 数据一致性
 * - RAM 占用显示格式（0KB / <1KB 字节 / KB 分级）
 * - 零 DRAM 功能折叠区分层展示
 * - 依赖联动隐藏（父功能禁用时子功能行隐藏）
 * - 未编译功能锁定态
 * - 保存并重启端到端往返（mdns 开关，模式无关、失败态安全）
 *
 * 设计原则：
 * - 自适应固件变体：能力缺失自动 skip（lite/standard/full/unified 均可运行）
 * - 非破坏性：除 FCFG-011 外均为本地切换不保存，不触发设备重启
 */

interface FeatureInfo {
  key: string;
  name: string;
  group: string;
  compiled: boolean;
  userEnabled: boolean;
  active: boolean;
  flashKB: number;
  ramB: number;
  hwDep?: string;
  dependsOn?: string;
  locked?: boolean;
}

interface FeatureStatus {
  features: FeatureInfo[];
  summary: {
    totalFlashKB: number;
    activeFlashKB: number;
    activeRamB: number;
    free8KB?: number;
    largest8KB?: number;
  };
}

/** 通过 API 获取功能状态（带 session cookie，行为断言的数据基准） */
async function fetchFeatureStatus(page: Page): Promise<FeatureStatus> {
  return page.evaluate(async () => {
    const r = await fetch('/api/features/status');
    const j = await r.json();
    return j.data;
  });
}

/** 等待设备重启并恢复：先探测健康检查失败（重启已开始），再等待恢复 */
async function waitForRebootAndRecovery(baseURL: string): Promise<boolean> {
  // 保存后设备约 3s 开始重启：轮询直到 /api/health 失败（最长 20s）
  const rebootStart = Date.now();
  while (Date.now() - rebootStart < 20_000) {
    try {
      const r = await fetch(`${baseURL}/api/health`, { signal: AbortSignal.timeout(2_000) });
      if (!r.ok) break;
    } catch {
      break; // 连接失败 = 重启已开始
    }
    await new Promise(r => setTimeout(r, 500));
  }
  return waitForHealth(baseURL, 120_000);
}

/** 重启后 session 可能失效：重载页面，必要时重新登录 */
async function reLoginIfNeeded(page: Page): Promise<void> {
  await page.goto('/', { waitUntil: 'domcontentloaded', timeout: 45_000 }).catch(() => {});
  const authed = await page.evaluate(() => {
    const app = document.querySelector('#app-container');
    const login = document.querySelector('#login-page') as HTMLElement | null;
    return !!app && !app.classList.contains('fb-hidden') && (!login || login.style.display === 'none');
  }).catch(() => false);
  if (authed) return;
  await page.waitForSelector('#login-page', { state: 'visible', timeout: 30_000 });
  await page.fill('#username', env.auth.username);
  await page.fill('#password', env.auth.password);
  await page.click('#login-button');
  await page.waitForSelector('#app-container', { state: 'visible', timeout: 40_000 });
}

/** 点击侧边栏菜单进入功能配置页并等待列表渲染 */
async function gotoFeaturesPage(page: Page): Promise<void> {
  await page.click('.menu-item[data-page="features"]', { timeout: 15_000 });
  await page.waitForSelector('#features-list [data-feature-key]', { state: 'attached', timeout: 20_000 });
}

/**
 * 在页面端按当前 locale 解析期望文案（镜像 feature-config.js 的 t() 回退链：
 * i18n 存在则取 i18n.t(key)，否则返回中文 fallback），避免硬编码中文断言
 * 在英文 locale 残留（如 AUTH-013 切换语言后）时误报
 */
async function resolveI18nText(page: Page, key: string, fallback: string): Promise<string> {
  return page.evaluate(([k, fb]) => {
    const w = window as unknown as { i18n?: { t?: (key: string) => string } };
    return (w.i18n && w.i18n.t) ? w.i18n.t(k) : fb;
  }, [key, fallback]);
}

test.describe('Suite-22: 功能配置页', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    test.skip(!(await hasMenuPage(authPage, 'features')), '设备不支持功能配置页面');
    await navigateTo('features');
    // 强制重新拉取最新状态，避免上一个测试的本地未保存修改残留
    await authPage.waitForSelector('#features-refresh-btn', { state: 'attached', timeout: 15_000 });
    await authPage.click('#features-refresh-btn');
    await authPage.waitForSelector('#features-list [data-feature-key]', { state: 'attached', timeout: 15_000 });
  });

  // ========== 页面结构与数据一致性 ==========

  test('FCFG-001: 进入功能配置页 @quick', async ({ authPage }) => {
    await expect(authPage.locator('#features-page')).toBeVisible();
  });

  test('FCFG-002: 工具栏与 RAM 监控卡片 @quick', async ({ authPage }) => {
    await expect(authPage.locator('#features-refresh-btn')).toBeVisible();
    const saveBtn = authPage.locator('#features-save-btn');
    await expect(saveBtn).toBeVisible();
    await expect(saveBtn).toBeDisabled(); // 初始无修改，保存按钮禁用
    await expect(authPage.locator('#ram-free8')).toBeVisible();
    await expect(authPage.locator('#ram-largest8')).toBeVisible();
    await expect(authPage.locator('#ram-active')).toBeVisible();
  });

  test('FCFG-003: 功能列表与 API 数据一致 @quick', async ({ authPage }) => {
    const status = await fetchFeatureStatus(authPage);
    expect(status.features.length).toBeGreaterThan(0);
    // 每个注册功能（含未编译锁定的）都渲染为一行
    const rowCount = await authPage.locator('#features-list [data-feature-key]').count();
    expect(rowCount).toBe(status.features.length);
    // 所有 API 键名都能在列表中找到
    const domKeys = await authPage.locator('#features-list [data-feature-key]')
      .evaluateAll(els => els.map(el => el.getAttribute('data-feature-key')));
    for (const f of status.features) {
      expect(domKeys, `功能 ${f.key} 应渲染`).toContain(f.key);
    }
  });

  // ========== RAM 显示格式（formatRam 分级） ==========

  test('FCFG-004: RAM 占用显示格式分级', async ({ authPage }) => {
    const status = await fetchFeatureStatus(authPage);
    for (const f of status.features) {
      const cell = authPage.locator(`tr[data-feature-key="${f.key}"] td:nth-child(3)`);
      const text = ((await cell.textContent()) || '').trim();
      const expected = f.ramB === 0
        ? '0KB'
        : (f.ramB < 1024 ? `${f.ramB}B` : `${(f.ramB / 1024).toFixed(1)}KB`);
      expect(text, `功能 ${f.key}（ramB=${f.ramB}）的 RAM 显示`).toBe(expected);
    }
  });

  // ========== 零 DRAM 折叠区分层展示 ==========

  test('FCFG-005: 零 DRAM 功能归入折叠区', async ({ authPage }) => {
    const status = await fetchFeatureStatus(authPage);
    const zeroRamKeys = status.features.filter(f => !f.ramB).map(f => f.key).sort();
    const section = authPage.locator('[data-zero-ram-toggle]');

    if (zeroRamKeys.length === 0) {
      // 无零 DRAM 功能时折叠区不应存在
      expect(await section.count()).toBe(0);
      return;
    }

    await expect(section).toBeVisible();
    // 折叠区内的行 = API 中 ramB===0 的功能集合
    const partition = await authPage.evaluate(() => {
      const rows = Array.from(document.querySelectorAll('#features-list [data-feature-key]'));
      const main: string[] = [];
      const zero: string[] = [];
      rows.forEach(r => {
        const key = r.getAttribute('data-feature-key') || '';
        (r.closest('#features-zero-ram-body') ? zero : main).push(key);
      });
      return { main, zero };
    });
    expect(partition.zero.sort()).toEqual(zeroRamKeys);
    // 主区域不应包含零 DRAM 功能
    for (const key of zeroRamKeys) {
      expect(partition.main).not.toContain(key);
    }
  });

  test('FCFG-006: 零 DRAM 折叠区展开/收起', async ({ authPage }) => {
    const header = authPage.locator('[data-zero-ram-toggle]');
    test.skip((await header.count()) === 0, '当前固件无零 DRAM 功能，无折叠区');
    const body = authPage.locator('#features-zero-ram-body');
    await expect(body).toBeHidden(); // 默认收起
    await header.click();
    await expect(body).toBeVisible();
    await header.click();
    await expect(body).toBeHidden();
  });

  // ========== 汇总与本地估算 ==========

  test('FCFG-007: 资源汇总与 RAM 监控填充', async ({ authPage }) => {
    const summaryText = (await authPage.locator('#features-summary').textContent()) || '';
    // 数值结构断言与语言无关（zh/en 均保留 Flash/RAM 技术词）
    expect(summaryText).toMatch(/Flash\s+\d+\s*KB\s*\|\s*RAM/);
    // RAM 监控卡片已由 API summary 填充（非占位符 --）
    await expect(authPage.locator('#ram-free8')).toHaveText(/\d+KB/);
    await expect(authPage.locator('#ram-largest8')).toHaveText(/\d+KB/);
    await expect(authPage.locator('#ram-active')).toHaveText(/\d+(\.\d+)?KB/);
  });

  test('FCFG-008: 本地切换开关使保存按钮可用（不保存）', async ({ authPage }) => {
    const status = await fetchFeatureStatus(authPage);
    // 选一个已编译、占 RAM、无依赖的管理入口之外功能做本地切换
    const candidate = status.features.find(f =>
      f.compiled && f.ramB > 0 && !f.dependsOn && f.key !== 'webServer' && f.key !== 'mdns');
    test.skip(!candidate, '无适合本地切换测试的功能');

    const row = authPage.locator(`tr[data-feature-key="${candidate!.key}"]`);
    await expect(authPage.locator('#features-save-btn')).toBeDisabled();
    await row.locator('.toggle-switch').click();
    await expect(authPage.locator('#features-save-btn')).toBeEnabled();
    // 本地估算文案更新（按当前 locale 解析期望前缀）
    const estimate = (await authPage.locator('#features-summary').textContent()) || '';
    const estimatePrefix = await resolveI18nText(authPage, 'fc-summary-flash', '预估启用资源: Flash');
    expect(estimate).toContain(estimatePrefix);
    // 切回（仍不保存，beforeEach 下次刷新即恢复）
    await row.locator('.toggle-switch').click();
  });

  // ========== 依赖联动隐藏 ==========

  test('FCFG-009: 父功能禁用时子功能行隐藏', async ({ authPage }) => {
    const status = await fetchFeatureStatus(authPage);
    // 找一个父子均编译入固件的依赖对
    const child = status.features.find(f => f.dependsOn && f.compiled
      && status.features.find(p => p.key === f.dependsOn)?.compiled);
    test.skip(!child, '当前固件无父子均编译的依赖对');
    const parentKey = child!.dependsOn!;
    const parent = status.features.find(f => f.key === parentKey)!;

    const parentToggle = authPage.locator(`tr[data-feature-key="${parentKey}"] .toggle-switch`);
    const childRow = authPage.locator(`tr[data-feature-key="${child!.key}"]`);

    // 前置：确保父功能处于启用态（子行可见）
    if (!parent.userEnabled) {
      await parentToggle.click();
    }
    await expect(childRow).toBeVisible();

    // 禁用父功能 → 子功能行隐藏
    await parentToggle.click();
    await expect(childRow).toBeHidden();

    // 恢复父功能 → 子功能行重新显示
    await parentToggle.click();
    await expect(childRow).toBeVisible();
    // 不保存：本地修改在下次进入页面刷新时丢弃
  });

  // ========== 未编译功能锁定 ==========

  test('FCFG-010: 未编译功能锁定不可启用', async ({ authPage }) => {
    const status = await fetchFeatureStatus(authPage);
    const locked = status.features.filter(f => !f.compiled);
    test.skip(locked.length === 0, '统一镜像已编入全部功能，无锁定项');
    const notCompiledText = await resolveI18nText(authPage, 'fc-not-compiled', '未编译入固件');
    for (const f of locked.slice(0, 3)) {
      const row = authPage.locator(`tr[data-feature-key="${f.key}"]`);
      await expect(row.locator('input[data-feature-toggle]')).toBeDisabled();
      await expect(row).toContainText(notCompiledText);
    }
  });

  // ========== 端到端：保存并重启往返 ==========

  test('FCFG-011: 保存并重启生效 — mdns 开关往返', async ({ authPage }) => {
    test.setTimeout(300_000); // 两次重启周期
    const baseURL = `http://${env.deviceIp}`;

    const status = await fetchFeatureStatus(authPage);
    const mdns = status.features.find(f => f.key === 'mdns');
    test.skip(!mdns || !mdns.compiled, 'mdns 未编入当前固件');
    const original = mdns!.userEnabled;
    const target = !original;

    // ── 第 1 程：切换到目标态并保存 ──
    await authPage.locator('tr[data-feature-key="mdns"] .toggle-switch').click();
    const saveBtn = authPage.locator('#features-save-btn');
    await expect(saveBtn).toBeEnabled();
    await saveBtn.click();

    expect(await waitForRebootAndRecovery(baseURL), '第 1 程：设备重启后应恢复').toBeTruthy();
    await reLoginIfNeeded(authPage);
    await gotoFeaturesPage(authPage);

    let current = await fetchFeatureStatus(authPage);
    expect(current.features.find(f => f.key === 'mdns')!.userEnabled,
      '第 1 程：mdns 状态应已翻转').toBe(target);

    // ── 第 2 程：恢复原始态并保存 ──
    await authPage.locator('tr[data-feature-key="mdns"] .toggle-switch').click();
    await authPage.locator('#features-save-btn').click();

    expect(await waitForRebootAndRecovery(baseURL), '第 2 程：设备重启后应恢复').toBeTruthy();
    await reLoginIfNeeded(authPage);
    await gotoFeaturesPage(authPage);

    current = await fetchFeatureStatus(authPage);
    expect(current.features.find(f => f.key === 'mdns')!.userEnabled,
      '第 2 程：mdns 状态应恢复原值').toBe(original);
  });
});
