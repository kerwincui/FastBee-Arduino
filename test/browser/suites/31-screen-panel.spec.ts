import { expect, test } from '../fixtures/base.fixture';
import { mkdirSync, readFileSync } from 'node:fs';
import path from 'node:path';
import type { Page } from '@playwright/test';

const artifactDir = path.resolve(__dirname, '..', 'artifacts', 'screen-panel');

async function resetPanel(page: Page): Promise<void> {
  const response = await page.request.post('/api/test/smart-panel-reset');
  expect(response.ok()).toBeTruthy();
}

async function openPanel(page: Page): Promise<void> {
  await page.waitForFunction(() => Boolean((window as unknown as { AppState?: unknown }).AppState));
  await page.waitForFunction(() => (window as unknown as {
    AppState?: { _featureStatusReady?: boolean };
  }).AppState?._featureStatusReady === true);
  await expect(page.locator('.menu-item[data-page="screen-panel"]')).toBeVisible();
  await page.evaluate(async () => {
    await (window as unknown as {
      AppState: { changePage: (name: string) => Promise<void> };
    }).AppState.changePage('screen-panel');
  });
  await page.locator('#screen-panel-page.active').waitFor({ state: 'visible' });
  await page.waitForFunction(() => (window as unknown as {
    AppState?: { _spModel?: { loaded?: boolean } };
  }).AppState?._spModel?.loaded === true);
}

async function openEditor(page: Page): Promise<void> {
  await page.locator('#sp-tab-config').click();
  if (!(await page.locator('#sp-layout-editor').evaluate((node: HTMLDetailsElement) => node.open))) {
    await page.locator('#sp-layout-editor > summary').click();
  }
}

async function smartState(page: Page): Promise<Record<string, any>> {
  const response = await page.request.get('/api/test/smart-panel-state');
  expect(response.ok()).toBeTruthy();
  return (await response.json()).data;
}

async function generateThreeScreens(page: Page): Promise<void> {
  await openEditor(page);
  await page.locator('#sp-generate-three-screen').click();
  await expect(page.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(4);
}

test.describe.serial('Device screen offline workflow', () => {

  test('WEB-PANEL-014: deployment evidence pins the field baseline contract used by UX V2 regressions', async () => {
    const evidenceRoot = path.resolve(__dirname, '..', '..', '..', 'artifacts', 'touch-optimization-20260922');
    const deployment = JSON.parse(readFileSync(path.join(evidenceRoot, 'deployment-COM8-r2.json'), 'utf-8'));
    const layout = JSON.parse(readFileSync(path.join(evidenceRoot, 'layout-after.json'), 'utf-8'));
    const api = JSON.parse(readFileSync(path.join(evidenceRoot, 'postdeploy-api.json'), 'utf-8'));
    const deployedConfig = layout.after;
    const screenConfig = api.checks.find((entry: any) => entry.route === '/api/screen-panel/config').data;
    const screenState = api.checks.find((entry: any) => entry.route === '/api/screen-panel/state').data;
    const weather = api.checks.find((entry: any) => entry.route === '/api/screen-panel/weather').data;

    expect.soft(deployment).toMatchObject({
      buildEnvironment: 'esp32s3-F16R8-rgb-xip40-v12',
      buildVariant: 'rgb-xip40-v12',
      firmwareSha256: 'cf616e14504308959128972ab0e68716d5b79a5eaa9ae04fdeb720f9866b0d60',
      webSha256: '3068b88e336a6f13e4b3683e887258b50ee4fed52c879fddd849c773c8d25193',
      webFileCount: 57,
      verified: true,
    });
    expect.soft(deployedConfig).toMatchObject({
      revision: 12,
      persisted: true,
      application: { applied: true, restartRequired: false, runtime: { running: true } },
      config: { displayRef: 'panel_display', settings: { brightnessPercent: 70, idleSleepSeconds: 0 } },
    });
    expect.soft(screenConfig.capabilities).toMatchObject({
      brightness: true, brightnessContinuous: true, sleep: true, buzzer: true,
      systemActions: ['brightness', 'sleep', 'buzzer', 'restart'],
    });
    expect.soft(screenState).toMatchObject({ revision: 12, available: true, brightnessPercent: 70, idleSleepSeconds: 0 });
    expect.soft(weather.status).toMatchObject({ status: 'error', error: 'Waiting for NTP before TLS', retryAfterMs: 0 });
    expect.soft(deployedConfig.config.pages.map((page: any) => page.id)).toEqual(['environment', 'trends', 'motor', 'controls', 'system']);
    expect.soft(deployedConfig.config.pages.find((page: any) => page.id === 'motor').cards).toHaveLength(1);
    expect.soft(deployedConfig.config.pages.find((page: any) => page.id === 'system').cards.map((card: any) => card.systemActionId))
      .toEqual(['brightness', 'sleep-time', 'sleep', 'buzzer', 'restart']);
  });

  test('WEB-PANEL-015: preview tab has no file/save operations and config tab has one primary save', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await expect(authPage.locator('#sp-preview')).toBeVisible();
    for (const selector of ['#sp-save-config', '#sp-import-config', '#sp-export-config', '#sp-reset-draft']) {
      await expect(authPage.locator(selector)).toBeHidden();
    }
    await authPage.locator('#sp-tab-config').click();
    await expect(authPage.locator('#sp-config')).toBeVisible();
    await expect(authPage.locator('#sp-save-config')).toBeVisible();
    await expect(authPage.locator('#sp-save-config:visible')).toHaveCount(1);
  });

  test('WEB-PANEL-016: five footer targets are large enough and switch directly in preview', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await generateThreeScreens(authPage);
    await authPage.locator('#sp-page-preset').selectOption('execute');
    await authPage.locator('#sp-page-add').click();
    await authPage.locator('#sp-tab-preview').click();
    await authPage.locator('#sp-preview-source').selectOption('draft');
    await authPage.setViewportSize({ width: 390, height: 844 });
    const footerButtons = authPage.locator('#sp-preview-canvas .sp-device-footer button');
    await expect(footerButtons).toHaveCount(5);
    const metrics = [] as Array<{ width: number; height: number }>;
    for (let index = 0; index < 5; index++) {
      metrics.push(await footerButtons.nth(index).evaluate((button: HTMLElement) => {
        const rect = button.getBoundingClientRect();
        return { width: rect.width, height: rect.height };
      }));
      await footerButtons.nth(index).click();
      await expect(footerButtons.nth(index)).toHaveClass(/is-active/);
    }
    for (const target of metrics) {
      expect(target.width).toBeGreaterThanOrEqual(48);
      expect(target.height).toBeGreaterThanOrEqual(48);
    }
  });
  test('WEB-PANEL-013: touch settings roundtrip and industrial previews never execute', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await generateThreeScreens(authPage);
    await authPage.locator('#sp-touch-beep').uncheck();
    await authPage.locator('#sp-confirm-idle').fill('120');
    await authPage.locator('#sp-confirm-idle').dispatchEvent('change');
    await authPage.locator('#sp-idle-sleep').selectOption('0');
    await authPage.locator('#sp-brightness').evaluate((input: HTMLInputElement) => {
      input.value = '0'; input.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await expect(authPage.locator('#sp-brightness-output')).toHaveText('最低亮度');
    await expect(authPage.locator('#sp-config')).toContainText('首次触摸不触发动作');
    const wire = await authPage.evaluate(() => {
      const app = (window as any).AppState;
      return app._screenPanelModelHelpers.sanitizeConfig(app._spModel.config, app._spModel.choices);
    });
    expect(wire.settings).toMatchObject({ brightnessPercent: 0, touchBeepEnabled: false, controlConfirmIdleSeconds: 120, idleSleepSeconds: 0, allowSystemRestart: false });
    expect(wire.pages.find((p: any) => p.id === 'system').cards.map((c: any) => c.systemActionId))
      .toEqual(['brightness', 'sleep-time', 'touch-beep', 'confirm-idle', 'sleep']);
    await authPage.locator('#sp-save-config').click();
    await expect(authPage.locator('#sp-dirty-state')).toHaveText('已保存');
    expect((await smartState(authPage)).screenPanelConfig.settings).toMatchObject(wire.settings);
    await authPage.locator('#sp-tab-preview').click();
    await authPage.locator('#sp-preview-pagination [data-page-id="motor"]').click();
    await expect(authPage.locator('.sp-motor-actions')).toHaveText('正转反转');
    await expect(authPage.locator('.sp-motor-stop')).toHaveText('停止');
    await expect(authPage.locator('#sp-preview-canvas')).not.toContainText('unknown');
    mkdirSync(artifactDir, { recursive: true });
    for (const width of [1440, 390]) {
      await authPage.setViewportSize({ width, height: 900 });
      for (const id of ['motor', 'system']) {
        await authPage.locator('#sp-preview-pagination [data-page-id="' + id + '"]').click();
        for (const control of await authPage.locator('#sp-preview-canvas .sp-panel-card button, #sp-preview-canvas .sp-panel-card input').all()) await expect(control).toBeDisabled();
        if (id === 'motor') {
          const rect = await authPage.locator('.sp-motor-stop').evaluate(button => {
            const stop = button.getBoundingClientRect(), card = button.closest('.sp-panel-card')!.getBoundingClientRect();
            return { stopBottom: stop.bottom, cardBottom: card.bottom, stopHeight: stop.height };
          });
          expect(rect.stopHeight).toBeGreaterThan(18);
          expect(rect.stopBottom).toBeLessThanOrEqual(rect.cardBottom);
        }
        await authPage.locator('#sp-preview-frame').screenshot({ path: path.join(artifactDir, `industrial-${id}-${width}.png`) });
      }
      expect(await authPage.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
    }
    await expect(authPage.locator('#sp-preview-canvas')).toContainText('最低亮度');
    await expect(authPage.locator('#sp-preview-canvas')).toContainText('永不');
    await expect(authPage.locator('#sp-preview-canvas [role="switch"]')).toHaveAttribute('aria-checked', 'false');
    await authPage.locator('#sp-footer-prev').click();
    await authPage.locator('#sp-footer-next').click();
    await authPage.locator('#sp-preview-home').click();
    await expect(authPage.locator('.sp-device-footer')).toContainText('1/4');
    const counts = (await smartState(authPage)).requestCounts;
    expect(counts['POST /api/screen-panel/execute'] || 0).toBe(0);
    expect(counts['POST /api/periph-exec/run'] || 0).toBe(0);
  });

  test('WEB-PANEL-008: templates append configurable pages and execute-once remains preview only', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await generateThreeScreens(authPage);
    const before = await authPage.evaluate(() => JSON.stringify((window as any).AppState._spModel.config.pages));
    await authPage.locator('#sp-page-preset').selectOption('execute');
    await authPage.locator('#sp-page-add').click();
    await expect(authPage.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(5);
    await expect(authPage.locator('#sp-page-limit')).toContainText('不固定三屏');
    const after = await authPage.evaluate(() => {
      const pages = (window as any).AppState._spModel.config.pages;
      return { original: JSON.stringify(pages.slice(0, 4)), page: pages[4] };
    });
    expect(after.original).toBe(before);
    expect(after.page.title).toBe('快捷执行');
    expect(after.page.cards.length).toBeGreaterThan(0);
    expect(after.page.cards.every((card: any) => card.kind === 'control' && card.display === 'action')).toBeTruthy();
    await authPage.locator('[data-sp-add="execute"]').click();
    await expect(authPage.locator('#sp-card-title')).toHaveValue('执行一次');
    await authPage.locator('#sp-tab-preview').click();
    await authPage.locator('#sp-preview-source').selectOption('draft');
    for (const button of await authPage.locator('#sp-preview-canvas .sp-panel-card button').all()) await expect(button).toBeDisabled();
    await authPage.locator('#sp-preview-home').click();
    await expect(authPage.locator('#sp-preview-canvas .sp-device-footer')).toContainText('1/');
    const counts = (await smartState(authPage)).requestCounts as Record<string, number>;
    expect(counts['POST /api/screen-panel/execute'] || 0).toBe(0);
    expect(counts['POST /api/periph-exec/run'] || 0).toBe(0);
  });

  test('WEB-PANEL-006: production DISPLAY descriptors support SPI selection and RGB roundtrip', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await authPage.locator('#add-peripheral-btn').click();
    const modal = authPage.locator('#peripheral-modal');
    await modal.locator('#peripheral-type-input').selectOption('56');
    await expect(modal.locator('#display-driver')).toHaveValue('st7789');
    await modal.locator('#display-driver').selectOption('ili9341');
    await modal.locator('#display-touch-backend').selectOption('gt911');
    await expect(modal.locator('#display-driver')).toHaveValue('ili9341');
    await expect(modal.locator('#display-touch-fields')).toBeVisible();
    await modal.locator('#display-backend').selectOption('rgb');
    await expect(modal.locator('#display-driver')).toHaveValue('st7701');
    await expect(modal.locator('#display-width')).toHaveValue('480');
    await expect(modal.locator('#display-height')).toHaveValue('480');
    await modal.locator('#display-rgb-fields > summary').click();
    await modal.locator('#peripheral-id-input').fill('ui_rgb_contract');
    await modal.locator('#peripheral-name-input').fill('RGB contract');
    const fields = { mosi: 1, sclk: 2, cs: 42, dc: -1, rgbDe: 40, rgbVsync: 39, rgbHsync: 38, rgbPclk: 41,
      controlSda: 15, controlScl: 7, controlAddress: 36, controlResetMask: 10, controlEnableMask: 32,
      controlBuzzerMask: 64, controlInactiveMask: 32, controlTouchIntMask: 4 };
    for (const [key, value] of Object.entries(fields)) await modal.locator('#display-' + key).fill(String(value));
    for (const [index, pin] of [5, 45, 48, 47, 21, 14, 13, 12, 11, 10, 9, 46, 3, 8, 18, 17].entries()) {
      await modal.locator('#display-rgbData' + index).fill(String(pin));
    }
    await modal.locator('#display-controlBackend').selectOption('tca9554');
    await modal.locator('#save-peripheral-btn').click();
    await expect(modal.locator('#peripheral-error')).toContainText('掩码必须互不重叠');
    await modal.locator('#display-controlInactiveMask').fill('0');
    await modal.locator('#save-peripheral-btn').click();
    await expect(modal.locator('#peripheral-error')).toContainText('蜂鸣器掩码仅支持 CH32V003');
    await modal.locator('#display-controlBackend').selectOption('ch32v003');
    const saved = authPage.waitForRequest(request => request.method() === 'POST' && new URL(request.url()).pathname === '/api/peripherals');
    await modal.locator('#save-peripheral-btn').click();
    const request = await saved;
    const payload = Object.fromEntries(new URLSearchParams(request.postData() || ''));
    expect(payload).toMatchObject({ displayBackend: 'rgb', displayDriver: 'st7701', width: '480', rgbData15: '17', controlBackend: 'ch32v003',
      controlBuzzerMask: '64', controlInactiveMask: '0', controlTouchIntMask: '4', dc: '-1' });
    await expect(modal).toHaveCount(0);
    await authPage.evaluate(() => (window as any).AppState.openPeripheralModal(true, 'ui_rgb_contract'));
    await expect(modal.locator('#display-backend')).toHaveValue('rgb');
    await expect(modal.locator('#display-controlBackend')).toHaveValue('ch32v003');
    await expect(modal.locator('#display-rgbData15')).toHaveValue('17');
    await expect(modal.locator('#display-controlResetMask')).toHaveValue('10');
    await expect(modal.locator('#display-controlBuzzerMask')).toHaveValue('64');
    await expect(modal.locator('#display-controlInactiveMask')).toHaveValue('0');
    await expect(modal.locator('#display-controlTouchIntMask')).toHaveValue('4');
    await modal.locator('#cancel-peripheral-btn').click();
  });

  test('WEB-PANEL-001: generated three-screen preview is responsive and never executes controls', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await expect(authPage.locator('#sp-tab-preview')).toHaveText('预览');
    await expect(authPage.locator('#sp-tab-config')).toHaveText('配置');
    await expect(authPage.locator('#sp-preview-canvas .sp-empty-surface')).toContainText('空页面');
    await expect(authPage.locator('#sp-editor-canvas')).toBeHidden();

    await generateThreeScreens(authPage);
    await expect(authPage.locator('#sp-page-tabs')).toContainText('首页');
    await expect(authPage.locator('#sp-page-tabs')).toContainText('温湿度趋势');
    await expect(authPage.locator('#sp-page-tabs')).toContainText('电机控制');

    await authPage.locator('#sp-tab-preview').click();
    await authPage.locator('#sp-preview-source').selectOption('draft');
    await expect(authPage.locator('#sp-preview-canvas .sp-weather-forecast .sp-weather-day')).toHaveCount(0);
    await expect(authPage.locator('#sp-preview-canvas .sp-weather-icon').first()).toHaveAttribute('data-icon-id', /sunny/);
    const weatherGlyphs = await authPage.evaluate(() => {
      const conditions = ['sunny', 'clear-night', 'cloudy', 'overcast', 'thunder', 'rain', 'snow', 'fog', 'wind', 'cold', 'hot', 'unknown'];
      const helpers = (window as any).AppState._screenPanelModelHelpers;
      const host = document.createElement('div');
      return Object.fromEntries(conditions.map((condition) => {
        host.innerHTML = helpers.weatherIconMarkup('', condition, (value: string) => value);
        return [condition, host.textContent];
      }));
    });
    expect(weatherGlyphs).toEqual({
      sunny: '☀', 'clear-night': '☾', cloudy: '☁', overcast: '☁', thunder: '⚡', rain: '☔',
      snow: '❄', fog: '☁', wind: '≈', cold: '❄', hot: '☀', unknown: '○',
    });
    await expect(authPage.locator('#sp-preview-canvas')).toContainText('上海');

    mkdirSync(artifactDir, { recursive: true });
    await authPage.setViewportSize({ width: 1440, height: 900 });
    await authPage.screenshot({ path: path.join(artifactDir, 'smart-panel-desktop.png'), fullPage: true });

    await authPage.locator('#sp-preview-next').click();
    await expect(authPage.locator('#sp-preview-canvas')).toContainText('锅炉温度');
    await authPage.locator('#sp-preview-next').click();
    await expect(authPage.locator('#sp-preview-canvas')).toContainText('传送电机');
    const previewActionButtons = authPage.locator('#sp-preview-canvas .sp-panel-card button');
    expect(await previewActionButtons.count()).toBeGreaterThan(0);
    for (const button of await previewActionButtons.all()) {
      await expect(button).toBeDisabled();
    }

    const counts = (await smartState(authPage)).requestCounts as Record<string, number>;
    expect(counts['POST /api/screen-panel/execute'] || 0).toBe(0);
    expect(counts['POST /api/periph-exec/run'] || 0).toBe(0);
    expect(counts['POST /api/system/restart'] || 0).toBe(0);

    await authPage.setViewportSize({ width: 390, height: 844 });
    await authPage.locator('#sp-preview-prev').click();
    await authPage.locator('#sp-preview-prev').click();
    const overflow = await authPage.evaluate(() => ({
      viewport: window.innerWidth,
      document: document.documentElement.scrollWidth,
      page: document.getElementById('screen-panel-page')?.scrollWidth || 0,
      pageClient: document.getElementById('screen-panel-page')?.clientWidth || 0,
    }));
    expect(overflow.document).toBeLessThanOrEqual(overflow.viewport + 1);
    expect(overflow.page).toBeLessThanOrEqual(overflow.pageClient + 1);
    const clock = authPage.locator('#sp-preview-canvas .sp-clock-value');
    expect(await clock.evaluate(node => node.scrollHeight <= node.clientHeight + 1)).toBeTruthy();
    await authPage.screenshot({ path: path.join(artifactDir, 'smart-panel-mobile.png'), fullPage: true });
  });

  test('WEB-PANEL-011: canvas handles stay quiet and page links fit narrow previews', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await generateThreeScreens(authPage);
    await authPage.locator('[data-sp-layout-mode="canvas"]').click();
    await expect(authPage.locator('#sp-editor-canvas')).toHaveClass(/is-canvas-mode/);
    await authPage.locator('[data-sp-add="page-link"]').click();

    const canvas = authPage.locator('#sp-editor-canvas');
    const pageLinkCard = canvas.locator('.sp-panel-card[data-kind="page-link"]');
    const selectedHandle = pageLinkCard.locator('.sp-resize-handle');
    const idleCardId = await canvas.locator('.sp-panel-card:not(.is-selected)').first().getAttribute('data-card-id');
    expect(idleCardId).toBeTruthy();
    const idleCard = canvas.locator(`.sp-panel-card[data-card-id="${idleCardId}"]`);
    const idleHandle = idleCard.locator('.sp-resize-handle');
    await expect(pageLinkCard).toHaveClass(/is-selected/);
    await expect(selectedHandle).toHaveCSS('display', 'block');
    await expect(idleHandle).toHaveCSS('display', 'none');
    await idleCard.click();
    await expect(idleCard).toHaveClass(/is-selected/);
    await expect(idleHandle).toHaveCSS('display', 'block');
    await expect(pageLinkCard).not.toHaveClass(/is-selected/);
    await expect(selectedHandle).toHaveCSS('display', 'none');

    mkdirSync(artifactDir, { recursive: true });
    const viewports = [
      { width: 1440, height: 900, file: 'preview-polish-desktop.png' },
      { width: 360, height: 800, file: 'preview-polish-360.png' },
      { width: 390, height: 844, file: 'preview-polish-390.png' },
    ];
    for (const viewport of viewports) {
      await authPage.setViewportSize({ width: viewport.width, height: viewport.height });
      await pageLinkCard.scrollIntoViewIfNeeded();
      const fit = await pageLinkCard.evaluate((card) => {
        const button = card.querySelector('.sp-page-link-preview button') as HTMLElement | null;
        const parent = button?.parentElement;
        const cardRect = card.getBoundingClientRect();
        const buttonRect = button?.getBoundingClientRect();
        return {
          buttonWidth: button?.clientWidth || 0,
          buttonScrollWidth: button?.scrollWidth || 0,
          buttonHeight: button?.clientHeight || 0,
          buttonScrollHeight: button?.scrollHeight || 0,
          parentWidth: parent?.clientWidth || 0,
          parentScrollWidth: parent?.scrollWidth || 0,
          insideCard: Boolean(buttonRect &&
            buttonRect.left >= cardRect.left - 1 && buttonRect.right <= cardRect.right + 1 &&
            buttonRect.top >= cardRect.top - 1 && buttonRect.bottom <= cardRect.bottom + 1),
        };
      });
      expect(fit.buttonWidth).toBeGreaterThan(0);
      expect(fit.buttonScrollWidth).toBeLessThanOrEqual(fit.buttonWidth + 1);
      expect(fit.buttonScrollHeight).toBeLessThanOrEqual(fit.buttonHeight + 1);
      expect(fit.parentScrollWidth).toBeLessThanOrEqual(fit.parentWidth + 1);
      expect(fit.insideCard).toBeTruthy();
      await authPage.screenshot({ path: path.join(artifactDir, viewport.file), fullPage: true });
    }
  });

  test('WEB-PANEL-002: wire import keeps rotation, icon and inverse drag in logical coordinates', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await openEditor(authPage);
    const beforePosts = ((await smartState(authPage)).requestCounts['POST /api/screen-panel/config'] || 0) as number;
    const imported = {
      storageVersion: 1,
      revision: 7,
      config: {
        schemaVersion: 1,
        displayRef: 'display_main',
        settings: {
          brightnessPercent: 70, idleSleepSeconds: 120, homePageId: 'home', themeId: 'high-contrast',
          background: { presetId: 'grid' }, allowSystemRestart: false,
        },
        pages: [{
          id: 'home', title: '旋转页', layoutId: 'detail', layoutMode: 'canvas',
          logicalWidth: 480, logicalHeight: 320, rotation: 90,
          cards: [{
            id: 'temperature', kind: 'data', visualType: 'value', title: '锅炉温度',
            sourceRef: 'modbus.boiler.temperature', unit: '°C', precision: 1,
            iconId: 'custom-icon', rect: { x: 24, y: 32, w: 160, h: 96 },
            requireConfirmation: false, historyMode: 'none',
          }],
        }],
      },
    };
    await authPage.locator('#sp-import-file').setInputFiles({
      name: 'rotated-panel.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify(imported)),
    });
    await expect(authPage.locator('#sp-page-message')).toContainText('布局已导入为草稿');
    await openEditor(authPage);
    const canvas = authPage.locator('#sp-editor-canvas');
    await expect(canvas).toHaveAttribute('data-logical-width', '480');
    await expect(canvas).toHaveAttribute('data-logical-height', '320');
    await expect(canvas).toHaveAttribute('data-output-width', '320');
    await expect(canvas).toHaveAttribute('data-output-height', '480');
    await expect(canvas).toHaveAttribute('data-rotation', '90');
    await expect(canvas.locator('.sp-card-icon[data-icon-id="custom-icon"]')).toBeVisible();
    const placement = await canvas.locator('.sp-panel-card').evaluate((element) => ({
      left: Number.parseFloat((element as HTMLElement).style.left),
      top: Number.parseFloat((element as HTMLElement).style.top),
      width: Number.parseFloat((element as HTMLElement).style.width),
      height: Number.parseFloat((element as HTMLElement).style.height),
    }));
    expect(placement.left).toBeCloseTo(60, 3);
    expect(placement.top).toBeCloseTo(5, 3);
    expect(placement.width).toBeCloseTo(30, 3);
    expect(placement.height).toBeCloseTo(100 / 3, 3);

    const beforeRect = await authPage.evaluate(() => structuredClone((window as any).AppState._spModel.config.pages[0].cards[0].rect));
    const draggableCard = canvas.locator('.sp-panel-card');
    await draggableCard.scrollIntoViewIfNeeded();
    const box = await draggableCard.boundingBox();
    expect(box).toBeTruthy();
    await authPage.mouse.move(box!.x + box!.width / 2, box!.y + box!.height / 2);
    await authPage.mouse.down();
    await authPage.mouse.move(box!.x + box!.width / 2 + 72, box!.y + box!.height / 2, { steps: 6 });
    await authPage.mouse.up();
    const afterRect = await authPage.evaluate(() => structuredClone((window as any).AppState._spModel.config.pages[0].cards[0].rect));
    expect(afterRect.y).toBeLessThan(beforeRect.y);
    expect(afterRect.x % 8).toBe(0);
    expect(afterRect.y % 8).toBe(0);

    const wire = await authPage.evaluate(() => {
      const app = (window as any).AppState;
      return app._screenPanelModelHelpers.sanitizeConfig(app._spModel.config, app._spModel.choices);
    });
    expect(wire.pages[0]).toMatchObject({ logicalWidth: 480, logicalHeight: 320, rotation: 90, layoutId: 'detail' });
    expect(wire.pages[0].cards[0]).toMatchObject({ kind: 'data', visualType: 'value', iconId: 'custom-icon' });
    expect(JSON.stringify(wire)).not.toMatch(/apiKey|authHeader|password|secret/i);
    const afterPosts = ((await smartState(authPage)).requestCounts['POST /api/screen-panel/config'] || 0) as number;
    expect(afterPosts).toBe(beforePosts);
  });

  test('WEB-PANEL-003: history settings persist unknown refs and 409 preserves the current draft', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await generateThreeScreens(authPage);
    await authPage.locator('#sp-tab-config').click();
    await authPage.locator('#sp-history-sources').evaluate((select) => {
      const selected = new Set(['modbus.boiler.temperature', 'can.motor.rpm']);
      for (const option of Array.from((select as HTMLSelectElement).options)) {
        option.selected = option.dataset.preserve === 'true' || selected.has(option.value);
      }
    });
    await authPage.locator('#sp-history-save').click();
    await expect(authPage.locator('#sp-page-message')).toContainText('最近记录设置已保存');
    let state = await smartState(authPage);
    expect(state.historyConfig.sourceAllowlist).toEqual(expect.arrayContaining([
      'modbus.boiler.temperature', 'can.motor.rpm', 'legacy.missing',
    ]));

    await authPage.locator('#sp-tab-preview').click();
    await authPage.locator('#sp-save-config').click();
    await expect(authPage.locator('#sp-page-message')).toContainText('配置已保存');
    state = await smartState(authPage);
    expect(state.screenPanelConfig.pages.map((entry: any) => entry.title)).toEqual(['首页', '温湿度趋势', '电机控制', '系统设置']);
    expect(state.screenPanelConfig.pages[0].cards.find((card: any) => card.kind === 'weather')).toMatchObject({ kind: 'weather', visualType: 'weather' });

    await openEditor(authPage);
    await authPage.locator('#sp-editor-canvas .sp-panel-card').first().click();
    await authPage.locator('#sp-card-title').fill('冲突后仍保留');
    await authPage.locator('#sp-card-apply').click();
    await authPage.request.post('/api/test/smart-panel-conflict');
    await authPage.locator('#sp-save-config').click();
    await expect(authPage.locator('#sp-page-message')).toContainText('另一窗口已保存新配置');
    await expect(authPage.locator('#sp-dirty-state')).toHaveText('有未保存修改');
    expect(await authPage.locator('#sp-editor-canvas .sp-panel-card').first().textContent()).toContain('冲突后仍保留');
    await authPage.waitForTimeout(220);
    expect(await authPage.evaluate(() => Boolean(localStorage.getItem('fastbee.screenPanel.draft.v1')))).toBe(true);
  });

  test('WEB-PANEL-009: compact information architecture and system actions serialize without Web execution', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await expect(authPage.locator('#menu-screen-panel')).toHaveText('设备屏幕');
    await expect(authPage.locator('.sp-top-tabs [role="tab"]')).toHaveCount(2);
    await expect(authPage.locator('#sp-editor-canvas')).toBeHidden();
    await expect(authPage.locator('#sp-display-ref')).toBeHidden();

    await authPage.locator('#sp-tab-preview').focus();
    await authPage.keyboard.press('ArrowRight');
    await expect(authPage.locator('#sp-tab-config')).toBeFocused();
    await expect(authPage.locator('#sp-tab-config')).toHaveAttribute('aria-selected', 'true');
    await expect(authPage.locator('#sp-display-ref')).toBeVisible();
    await authPage.keyboard.press('ArrowLeft');
    await expect(authPage.locator('#sp-tab-preview')).toBeFocused();
    await expect(authPage.locator('#sp-editor-canvas')).toBeHidden();

    await openEditor(authPage);
    await authPage.locator('[data-sp-add="brightness"]').click();
    await authPage.locator('#sp-brightness').evaluate((input) => {
      (input as HTMLInputElement).value = '42';
      input.dispatchEvent(new Event('change', { bubbles: true }));
    });
    await authPage.locator('#sp-card-apply').click();
    await authPage.locator('[data-sp-add="sleep"]').click();
    await authPage.locator('[data-sp-add="buzzer"]').click();
    await authPage.locator('#sp-tab-config').click();
    await expect(authPage.locator('#sp-display-ref')).toBeVisible();
    await expect(authPage.locator('#sp-system-capabilities')).toContainText('亮度');
    await authPage.locator('#sp-allow-restart').check();
    await openEditor(authPage);
    await authPage.locator('[data-sp-add="restart"]').click();

    const wire = await authPage.evaluate(() => {
      const app = (window as any).AppState;
      return app._screenPanelModelHelpers.sanitizeConfig(app._spModel.config, app._spModel.choices);
    });
    const systemCards = wire.pages[0].cards.filter((card: any) => card.kind === 'system');
    expect(systemCards.map((card: any) => card.systemActionId)).toEqual(['brightness', 'sleep', 'buzzer', 'restart']);
    expect(systemCards[0]).toMatchObject({ visualType: 'slider' });
    expect(wire.settings.brightnessPercent).toBe(42);
    expect(systemCards[1]).not.toHaveProperty('actionValue');
    expect(systemCards[2]).not.toHaveProperty('actionValue');
    expect(systemCards[3]).toMatchObject({ requireConfirmation: true });

    const counts = (await smartState(authPage)).requestCounts as Record<string, number>;
    expect(counts['POST /api/screen-panel/execute'] || 0).toBe(0);
    expect(counts['POST /api/system/restart'] || 0).toBe(0);

    await authPage.setViewportSize({ width: 390, height: 844 });
    const mobileLayout = await authPage.evaluate(() => {
      const canvas = document.querySelector('.sp-canvas-column')!.getBoundingClientRect();
      const toolbox = document.querySelector('.sp-toolbox')!.getBoundingClientRect();
      return {
        canvasTop: canvas.top,
        toolboxTop: toolbox.top,
        viewportWidth: window.innerWidth,
        documentWidth: document.documentElement.scrollWidth,
      };
    });
    expect(mobileLayout.canvasTop).toBeLessThan(mobileLayout.toolboxTop);
    expect(mobileLayout.documentWidth).toBeLessThanOrEqual(mobileLayout.viewportWidth + 1);
  });

  test('WEB-PANEL-004: remote secrets and DISPLAY/GT911 fields round-trip without disclosure', async ({ authPage }) => {
    await resetPanel(authPage);
    const update = await authPage.request.post('/api/peripherals/update', {
      data: {
        id: 'display_main', name: '横向触摸屏', type: 56, enabled: true, pins: '',
        displayBackend: 'spi_tft', displayDriver: 'ili9341', width: 320, height: 240, rotation: 3,
        touchBackend: 'gt911', touchSda: 19, touchScl: 45, touchInt: -1, touchRst: -1,
      },
    });
    expect(update.ok()).toBeTruthy();
    const peripheral = await (await authPage.request.get('/api/peripherals?id=display_main')).json();
    expect(peripheral.data.params).toMatchObject({
      backend: 'spi_tft', driver: 'ili9341', width: 320, height: 240, rotation: 3,
      touchBackend: 'gt911', pins: { touchSda: 19, touchScl: 45, touchInt: -1, touchRst: -1 },
    });

    await openPanel(authPage);
    await authPage.locator('#sp-tab-config').click();
    await expect(authPage.locator('#sp-display-ref option[value="display_main"]')).toContainText('320×240');
    await authPage.locator('#sp-display-ref').selectOption('');
    await authPage.locator('#sp-display-ref').selectOption('display_main');
    const displayBinding = await authPage.evaluate(() => {
      const page = (window as any).AppState._spModel.config.pages[0];
      return { canvas: page.canvas, rotation: page.rotation };
    });
    expect(displayBinding).toEqual({ canvas: { width: 320, height: 240 }, rotation: 270 });
    await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-output-width', '240');
    await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-output-height', '320');

    await expect(authPage.locator('#sp-weather-key')).toHaveValue('');
    await expect(authPage.locator('#sp-weather-interval')).toHaveValue('1800');
    await expect(authPage.locator('#sp-weather-key')).toHaveAttribute('placeholder', '已设置，留空保留');
    await authPage.locator('#sp-weather-location').fill('beijing');
    const posted = authPage.waitForRequest((request) =>
      request.method() === 'POST' && new URL(request.url()).pathname === '/api/screen-panel/weather');
    await authPage.locator('#sp-save-config').click();
    expect((await posted).postDataJSON()).toMatchObject({ apiKey: '', location: 'beijing', expectedRevision: 1 });
    await expect(authPage.locator('#sp-page-message')).toContainText('配置已保存');
    expect((await smartState(authPage)).screenWeather).toMatchObject({ hasApiKey: true, location: 'beijing', revision: 2 });
    await authPage.locator('#sp-weather-key').fill('never-persist-this-key');
    await authPage.locator('#sp-theme-id').selectOption('light');
    await authPage.waitForTimeout(250);
    const stored = await authPage.evaluate(() => localStorage.getItem('fastbee.screenPanel.draft.v1') || '');
    expect(stored).not.toContain('never-persist-this-key');
    await authPage.locator('#sp-refresh-btn').click();
    await expect(authPage.locator('#sp-weather-key')).toHaveValue('never-persist-this-key');
  });

  test('WEB-PANEL-010: weather refresh is bounded and displays normalized current conditions', async ({ authPage }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    await authPage.locator('#sp-tab-config').click();
    const posted = authPage.waitForRequest((request) =>
      request.method() === 'POST' && new URL(request.url()).pathname === '/api/screen-panel/weather/refresh');
    await authPage.locator('#sp-weather-refresh').click();
    expect((await posted).postData()).toBe('id=main');
    await expect(authPage.locator('#sp-weather-status')).toContainText('26.5℃');
    await expect(authPage.locator('#sp-weather-status')).toContainText('上海');
    await expect(authPage.locator('#sp-weather-status')).not.toContainText('offline-secret');
    await expect(authPage.locator('#sp-weather-refresh')).toBeEnabled();
    await authPage.locator('#sp-weather-refresh').click();
    await expect(authPage.locator('#sp-page-message')).toContainText('秒后可再次更新');
    const state = await smartState(authPage);
    expect(state.requestCounts['POST /api/screen-panel/weather/refresh']).toBe(2);
    expect(state.requestCounts['GET /api/screen-panel/weather/refresh']).toBeGreaterThanOrEqual(1);
  });

  test('WEB-PANEL-012: menu order, remount, motor settings and partial save remain coherent', async ({ authPage, navigateTo }) => {
    await resetPanel(authPage);
    await openPanel(authPage);
    const order = await authPage.locator('.menu-item[data-page]').evaluateAll(nodes => nodes.map(node => node.getAttribute('data-page')));
    expect(order.indexOf('screen-panel')).toBe(order.indexOf('protocol') + 1);
    await navigateTo('protocol');
    await expect(authPage.locator('.config-tab[data-tab="remote-resources"]')).toHaveCount(0);
    await openPanel(authPage);
    await authPage.evaluate(async () => {
      const root = document.getElementById('screen-panel-page')!; root.replaceWith(root.cloneNode(true));
      await (window as any).AppState.loadScreenPanelPage({ noCache: true });
    });
    await generateThreeScreens(authPage);
    await authPage.locator('#sp-page-tabs [data-page-id="motor"]').click();
    await authPage.locator('#sp-editor-canvas [data-kind="motor"]').click();
    await authPage.locator('#sp-motor-speed').fill('0');
    await authPage.locator('#sp-motor-pulse').fill('999');
    await authPage.locator('#sp-card-apply').click();
    await expect(authPage.locator('#sp-motor-speed')).toHaveValue('0');
    await authPage.locator('#sp-weather-location').fill('beijing');
    await authPage.request.post('/api/test/smart-panel-conflict');
    await authPage.locator('#sp-save-config').click();
    await expect(authPage.locator('#sp-page-message')).toContainText(/天气.*已保存/);
    await expect(authPage.locator('#sp-page-message')).toContainText('另一窗口已保存新配置');
    await expect(authPage.locator('#sp-dirty-state')).toHaveText('有未保存修改');
    expect((await smartState(authPage)).screenWeather.location).toBe('beijing');
    const motor = await authPage.evaluate(() => {
      const app = (window as any).AppState;
      return app._screenPanelModelHelpers.sanitizeConfig(app._spModel.config, app._spModel.choices).pages.find((p: any) => p.id === 'motor').cards[0];
    });
    expect(motor).toMatchObject({ kind: 'control', visualType: 'motor', motorSpeed: 0, motorPulse: 999 });
    mkdirSync(artifactDir, { recursive: true });
    await authPage.locator('#sp-editor-canvas').screenshot({ path: path.join(artifactDir, 'motor-preview.png') });
    await authPage.locator('#sp-layout-editor > summary').click();
    await authPage.locator('#sp-weather-heading').scrollIntoViewIfNeeded();
    await authPage.screenshot({ path: path.join(artifactDir, 'config-desktop.png') });
    await authPage.setViewportSize({ width: 390, height: 844 });
    await authPage.locator('#sp-weather-heading').scrollIntoViewIfNeeded();
    await authPage.screenshot({ path: path.join(artifactDir, 'config-mobile.png') });
    expect(await authPage.evaluate(() => document.documentElement.scrollWidth)).toBeLessThanOrEqual(391);
  });
});

