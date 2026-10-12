import { env, expect, test as baseTest } from '../fixtures/base.fixture';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import type { Page } from '@playwright/test';

const test = baseTest.extend<{ offlinePanelGuard: void }>({
  offlinePanelGuard: [async ({ baseURL }, use) => {
    const target = new URL(baseURL || env.baseURL);
    expect(['127.0.0.1', '127.0.0.2', 'localhost', '[::1]'], 'Panel fixtures must not mutate a real device').toContain(target.hostname);
    expect(new URL(env.baseURL).origin).toBe(target.origin);
    await use();
  }, { auto: true }],
});

async function resetPanel(page: Page): Promise<void> {
  expect(['127.0.0.1', '127.0.0.2', 'localhost', '[::1]']).toContain(new URL(page.url()).hostname);
  const response = await page.request.post('/api/test/smart-panel-reset');
  expect(response.ok()).toBeTruthy();
}

async function openPanel(page: Page): Promise<void> {
  if (process.env.FASTBEE_PANEL_SOURCE === '1') {
    const root = path.resolve(__dirname, '../../..');
    const assets = { 'pages/screen-panel.html': 'web-src/pages/screen-panel.html', 'css/screen-panel.css': 'web-src/css/screen-panel.css', 'js/modules/screen-panel.js': 'web-src/modules/runtime/screen-panel.js', 'js/modules/screen-panel-model.js': 'web-src/modules/runtime/screen-panel-model.js' };
    for (const [url, file] of Object.entries(assets)) await page.route('**/' + url + '*', route => route.fulfill({ body: readFileSync(path.join(root, file)), contentType: file.endsWith('.js') ? 'application/javascript' : file.endsWith('.css') ? 'text/css' : 'text/html' }));
  }
  await page.evaluate(() => { Object.keys(localStorage).filter(key => key.startsWith('fastbee.screenPanel.draft.')).forEach(key => localStorage.removeItem(key)); });
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
  await expect(page.locator('#sp-preview, #sp-tab-config, #sp-tab-preview')).toHaveCount(0);
  await expect(page.locator('#sp-editor-canvas')).toBeVisible();
}

async function smartState(page: Page): Promise<Record<string, any>> {
  const response = await page.request.get('/api/test/smart-panel-state', { maxRetries: 1 });
  expect(response.ok()).toBeTruthy();
  return (await response.json()).data;
}

async function changeSavedPanel(page: Page): Promise<void> {
  const state = await smartState(page);
  const config = structuredClone(state.screenPanelConfig);
  config.pages[0].title = 'Changed on device';
  const response = await page.request.post('/api/screen-panel/config', {
    data: { expectedRevision: state.screenPanelRevision, config },
  });
  expect(response.ok()).toBeTruthy();
}

async function generateThreeScreens(page: Page): Promise<void> {
  await openEditor(page);
  await page.locator('#sp-generate-three-screen').click();
  await expect(page.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(5);
}


async function savePanel(page: Page): Promise<void> {
  await page.locator('#sp-save-config').click();
  await expect(page.locator('#sp-dirty-state')).toHaveText('已保存');
}
async function selectComponent(page: Page, pageId: string, kind: string): Promise<void> {
  await openEditor(page);
  await page.locator(`#sp-page-tabs [data-page-id="${pageId}"]`).click();
  await page.locator(`#sp-editor-canvas [data-kind="${kind}"]`).first().click();
}

async function expectDraftOnly(page: Page): Promise<void> {
  const counts = (await smartState(page)).requestCounts;
  const mutations = Object.keys(counts).filter(key => /^(POST|PUT|PATCH|DELETE) /.test(key)
    && key !== 'POST /api/batch' && !key.startsWith('POST /api/auth/') && !key.startsWith('POST /api/test/') && counts[key] > 0);
  expect(mutations, 'Editor interactions must not save or actuate without an explicit command').toEqual([]);
}

test.describe('Device screen offline workflow', () => {
  test('WEB-PANEL-ORDER-001: page drag keeps home and selection, supports undo and persists', async ({ authPage: page }, testInfo) => {
    await resetPanel(page); await openPanel(page); await generateThreeScreens(page);
    const before = await page.evaluate(() => {
      const m = (window as any).AppState._spModel;
      return { ids: m.config.pages.map((p: any) => p.id), home: m.config.settings.homePageId,
        selected: document.querySelector('#sp-page-tabs .is-active')?.getAttribute('data-page-id') };
    });
    const source = page.locator('#sp-page-tabs [data-page-id="' + before.ids[0] + '"]');
    const target = page.locator('#sp-page-tabs [data-page-id="' + before.ids[2] + '"]');
    await source.dragTo(target);
    const expected = before.ids.slice(); expected.splice(2, 0, expected.splice(0, 1)[0]);
    await expect.poll(() => page.evaluate(() => (window as any).AppState._spModel.config.pages.map((p: any) => p.id))).toEqual(expected);
    await expectDraftOnly(page);
    await page.locator('#sp-undo').click();
    await expect.poll(() => page.evaluate(() => (window as any).AppState._spModel.config.pages.map((p: any) => p.id))).toEqual(before.ids);
    await page.locator('#sp-redo').click();
    await expect(page.locator('#sp-page-tabs .is-active')).toHaveAttribute('data-page-id', before.selected!);
    await savePanel(page);
    const saved = (await smartState(page)).screenPanelConfig;
    expect(saved.pages.map((p: any) => p.id)).toEqual(expected);
    expect(saved.settings.homePageId).toBe(before.home);
    await page.reload(); await openPanel(page);
    expect(await page.locator('#sp-page-tabs .sp-page-tab').evaluateAll(nodes => nodes.map(n => (n as HTMLElement).dataset.pageId))).toEqual(expected);
    expect((await smartState(page)).requestCounts['POST /api/screen-panel/execute'] || 0).toBe(0);
    await page.screenshot({ path: testInfo.outputPath('page-order-desktop.png') });
  });

  test('WEB-PANEL-ORDER-002: template component drag repacks without changing bindings or executing', async ({ authPage: page }) => {
    await resetPanel(page); await openPanel(page); await generateThreeScreens(page);
    await page.locator('#sp-page-tabs [data-page-id="system"]').click();
    const original = await page.evaluate(() => structuredClone((window as any).AppState._spModel.config.pages.find((p: any) => p.id === 'system').cards));
    const from = original[original.length - 1].id, to = original[0].id;
    await page.locator('#sp-editor-canvas [data-card-id="' + from + '"]').dragTo(
      page.locator('#sp-editor-canvas [data-card-id="' + to + '"]'),
      { sourcePosition: { x: 12, y: 12 }, targetPosition: { x: 12, y: 12 } });
    const expected = original.map((c: any) => c.id); expected.unshift(expected.pop());
    await expect.poll(() => page.evaluate(() => (window as any).AppState._spModel.config.pages.find((p: any) => p.id === 'system').cards.map((c: any) => c.id))).toEqual(expected);
    await expect(page.locator('#sp-layout-error')).toBeHidden();
    await expectDraftOnly(page);
    const after = await page.evaluate(() => (window as any).AppState._spModel.config.pages.find((p: any) => p.id === 'system').cards);
    const bindings = (cards: any[]) => cards.map(({ rect, ...rest }) => rest).sort((a, b) => a.id.localeCompare(b.id));
    expect(bindings(after)).toEqual(bindings(original));
    await page.locator('#sp-undo').click();
    expect(await page.evaluate(() => (window as any).AppState._spModel.config.pages.find((p: any) => p.id === 'system').cards)).toEqual(original);
    await page.locator('#sp-redo').click(); await savePanel(page);
    await page.reload(); await openPanel(page);
    await page.locator('#sp-page-tabs [data-page-id="system"]').click();
    expect(await page.evaluate(() => (window as any).AppState._spModel.config.pages.find((p: any) => p.id === 'system').cards.map((c: any) => c.id))).toEqual(expected);
    expect((await smartState(page)).requestCounts['POST /api/screen-panel/execute'] || 0).toBe(0);
  });

  test('WEB-PANEL-ORDER-003: escape and outside drop cancel without a new undo entry', async ({ authPage: page }) => {
    await resetPanel(page); await openPanel(page);
    const initial = await page.evaluate(() => structuredClone((window as any).AppState._spModel.config));
    await generateThreeScreens(page);
    const before = await page.evaluate(() => structuredClone((window as any).AppState._spModel.config));
    const a = await page.locator('#sp-page-tabs .sp-page-tab').first().boundingBox();
    const b = await page.locator('#sp-page-tabs .sp-page-tab').nth(2).boundingBox();
    expect(a && b).toBeTruthy();
    for (const cancel of ['escape', 'outside']) {
      await page.mouse.move(a!.x + 12, a!.y + 12); await page.mouse.down();
      await page.mouse.move(b!.x + 12, b!.y + 12, { steps: 6 });
      if (cancel === 'escape') await page.keyboard.press('Escape');
      else await page.mouse.move(2, 2, { steps: 4 });
      await page.mouse.up();
      expect(await page.evaluate(() => (window as any).AppState._spModel.config)).toEqual(before);
      await expect(page.locator('.is-sorting, .is-sort-target')).toHaveCount(0);
    }
    await page.locator('#sp-undo').click();
    expect(await page.evaluate(() => (window as any).AppState._spModel.config)).toEqual(initial);
    await expectDraftOnly(page);
  });

  test('WEB-PANEL-ORDER-004: mobile touch handle changes page order without scrolling the canvas', async ({ authPage: page }) => {
    await page.setViewportSize({ width: 390, height: 844 });
    await resetPanel(page); await openPanel(page); await generateThreeScreens(page);
    const before = await page.evaluate(() => (window as any).AppState._spModel.config.pages.map((p: any) => p.id));
    const a = await page.locator('#sp-page-tabs .sp-sort-handle').first().boundingBox();
    const b = await page.locator('#sp-page-tabs .sp-sort-handle').nth(2).boundingBox();
    expect(a && b).toBeTruthy();
    const cdp = await page.context().newCDPSession(page);
    await cdp.send('Emulation.setTouchEmulationEnabled', { enabled: true, maxTouchPoints: 1 });
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: a!.x + 8, y: a!.y + 8 }] });
    for (let step = 1; step <= 8; step++) await cdp.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: a!.x + 8 + (b!.x - a!.x) * step / 8, y: a!.y + 8 + (b!.y - a!.y) * step / 8 }] });
    await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
    const expected = before.slice(); expected.splice(2, 0, expected.splice(0, 1)[0]);
    await expect.poll(() => page.evaluate(() => (window as any).AppState._spModel.config.pages.map((p: any) => p.id))).toEqual(expected);
    await cdp.detach(); await expectDraftOnly(page);
  });

  test('WEB-PANEL-SLEEP-001: explicit switch persists off, restores delay and never executes sleep', async ({ authPage: page }, testInfo) => {
    await resetPanel(page); await openPanel(page);
    await page.locator('#sp-global-settings').click();
    await expect(page.locator('#sp-auto-sleep-enabled')).toBeChecked();
    await page.locator('#sp-idle-sleep').selectOption('300');
    await page.locator('#sp-auto-sleep-enabled').uncheck();
    await expect(page.locator('#sp-idle-sleep')).toBeDisabled();
    await page.locator('#sp-auto-sleep-enabled').check();
    await expect(page.locator('#sp-idle-sleep')).toHaveValue('300');
    await page.locator('#sp-auto-sleep-enabled').uncheck();
    await expectDraftOnly(page); await savePanel(page);
    expect((await smartState(page)).screenPanelConfig.settings.idleSleepSeconds).toBe(0);
    await page.reload(); await openPanel(page); await page.locator('#sp-global-settings').click();
    await expect(page.locator('#sp-auto-sleep-enabled')).not.toBeChecked();
    await expect(page.locator('#sp-idle-sleep')).toBeDisabled();
    await page.setViewportSize({ width: 390, height: 844 });
    await page.locator('#sp-properties-close').click();
    await page.locator('#sp-global-settings').click();
    await expect(page.locator('#sp-auto-sleep-enabled')).toBeVisible();
    await page.screenshot({ path: testInfo.outputPath('auto-sleep-mobile.png') });
    expect((await smartState(page)).requestCounts['POST /api/screen-panel/execute'] || 0).toBe(0);
  });

  for (const kind of ['data', 'status', 'gauge', 'bar', 'line', 'clock', 'weather', 'page-link', 'motor',
      'control', 'execute', 'brightness', 'sleep-time', 'sleep', 'buzzer', 'touch-beep', 'confirm-idle', 'restart']) {
    test('WEB-PANEL-033: ' + kind + ' drag configure save reload without actuation', async ({ authPage: page }) => {
      await resetPanel(page); await openPanel(page);
      const errors: string[] = []; page.on('pageerror', error => errors.push(error.message));
      if (kind === 'restart') {
        await page.locator('#sp-global-settings').click(); await page.locator('#sp-allow-restart').check();
      }
      const button = page.locator('[data-sp-add="' + kind + '"]');
      await button.scrollIntoViewIfNeeded(); await expect(button).toBeEnabled();
      await button.dragTo(page.locator('#sp-editor-canvas'), { targetPosition: { x: 100, y: 100 } });
      await expect(page.locator('#sp-editor-canvas .sp-panel-card')).toHaveCount(1);
      await page.locator('#sp-card-title').fill('Configured ' + kind);
      if (['data', 'status', 'gauge', 'bar', 'line'].includes(kind)) {
        await page.locator('#sp-card-source').selectOption('modbus.boiler.temperature');
      }
      if (['gauge', 'bar'].includes(kind)) {
        await page.locator('#sp-card-min').fill('-12.5'); await page.locator('#sp-card-max').fill('82.5');
      }
      if (kind === 'control' || kind === 'execute') {
        await page.locator('#sp-card-control').selectOption(kind === 'control' ? 'rule.living-light' : 'rule.away-scene');
        await page.locator('#sp-card-confirm').uncheck();
      }
      await expectDraftOnly(page); await savePanel(page);
      const saved = (await smartState(page)).screenPanelConfig.pages[0].cards[0];
      expect(saved.title).toBe('Configured ' + kind);
      if (['gauge', 'bar'].includes(kind)) expect(saved).toMatchObject({rangeMin: -12.5, rangeMax: 82.5});
      await page.evaluate(async () => (window as any).AppState.loadScreenPanelPage({ noCache: true, preserveDraft: false }));
      await page.locator('#sp-editor-canvas .sp-panel-card').click();
      await expect(page.locator('#sp-card-title')).toHaveValue('Configured ' + kind);
      if (['gauge', 'bar'].includes(kind)) {
        await expect(page.locator('#sp-card-min')).toHaveValue('-12.5'); await expect(page.locator('#sp-card-max')).toHaveValue('82.5');
      }
      if (kind === 'control' || kind === 'execute') await expect(page.locator('#sp-card-confirm')).not.toBeChecked();
      const counts = (await smartState(page)).requestCounts;
      expect(Object.keys(counts).filter(key => key.startsWith('POST ') && /\/run|\/action|\/command|\/execute/.test(key) && counts[key])).toEqual([]);
      expect(errors).toEqual([]);
    });
  }

  test('WEB-PANEL-034: reload offers a draft after a device setting revision change and preserves its baseline', async ({ authPage: page }) => {
    await resetPanel(page); await openPanel(page);
    await page.locator('[data-sp-add="clock"]').click(); await page.locator('#sp-card-title').fill('Recover across revision');
    await expect.poll(() => page.evaluate(() => Object.keys(localStorage).filter(k => k.startsWith('fastbee.screenPanel.draft.')).length)).toBe(1);
    const state = await smartState(page), remote = structuredClone(state.screenPanelConfig);
    remote.settings.brightnessPercent = 52;
    expect((await page.request.post('/api/screen-panel/config', {data:{expectedRevision:state.screenPanelRevision,config:remote}})).ok()).toBeTruthy();
    await page.reload();
    await page.waitForFunction(() => Boolean((window as any).AppState?._featureStatusReady));
    await page.evaluate(async () => (window as any).AppState.changePage('screen-panel'));
    await expect(page.locator('#sp-draft-recovery')).toBeVisible();
    await page.locator('#sp-draft-restore').click();
    await expect(page.locator('#sp-editor-canvas')).toContainText('Recover across revision');
    await savePanel(page);
    const saved = (await smartState(page)).screenPanelConfig;
    expect(saved.settings.brightnessPercent).toBe(52);
    expect(saved.pages[0].cards[0].title).toBe('Recover across revision');
  });

  test('WEB-PANEL-037: reload before draft debounce preserves the latest edit', async ({ authPage: page }) => {
    await resetPanel(page); await openPanel(page);
    await page.locator('[data-sp-add="clock"]').click(); await savePanel(page);
    // Hold only the editor debounce, leaving UI/network timers operational.
    await page.evaluate(() => {
      const original = window.setTimeout.bind(window);
      window.setTimeout = ((handler: TimerHandler, delay?: number, ...args: any[]) =>
        original(handler, delay === 160 ? 60000 : delay, ...args)) as typeof window.setTimeout;
    });
    await page.locator('#sp-card-title').fill('Pending departure edit');
    await expect(page.locator('#sp-dirty-state')).toHaveText('有未保存修改');
    expect(await page.evaluate(() => Object.keys(localStorage)
      .filter(key => key.startsWith('fastbee.screenPanel.draft.'))
      .some(key => (localStorage.getItem(key) || '').includes('Pending departure edit')))).toBeFalsy();
    await page.reload();
    await page.waitForFunction(() => Boolean((window as any).AppState?._featureStatusReady));
    await page.evaluate(async () => (window as any).AppState.changePage('screen-panel'));
    await expect(page.locator('#sp-draft-recovery')).toBeVisible();
    await page.locator('#sp-draft-restore').click();
    await expect(page.locator('#sp-editor-canvas')).toContainText('Pending departure edit');
    await savePanel(page);
    expect((await smartState(page)).screenPanelConfig.pages[0].cards[0].title).toBe('Pending departure edit');
  });

  test('WEB-PANEL-031: concurrent device settings merge while a genuine layout conflict preserves the draft', async ({ authPage: page }) => {
    await resetPanel(page); await openPanel(page);
    await page.locator('[data-sp-add="clock"]').click();
    await savePanel(page);
    await page.locator('#sp-card-title').fill('Local clock');
    const state = await smartState(page);
    const remote = structuredClone(state.screenPanelConfig);
    remote.settings.brightnessPercent = 52;
    const result = await page.request.post('/api/screen-panel/config', { data: { expectedRevision: state.screenPanelRevision, config: remote } });
    expect(result.ok()).toBeTruthy();
    await savePanel(page);
    const merged = await smartState(page);
    expect(merged.screenPanelConfig.settings.brightnessPercent).toBe(52);
    expect(merged.screenPanelConfig.pages[0].cards[0].title).toBe('Local clock');
    await page.locator('#sp-card-title').fill('Keep my draft');
    await changeSavedPanel(page);
    await page.locator('#sp-save-config').click();
    await expect(page.locator('#sp-page-message')).toContainText('设备配置已更新');
    await expect(page.locator('#sp-dirty-state')).toHaveText('有未保存修改');
    await page.locator('#sp-refresh-btn').click();
    await expect(page.locator('#sp-card-title')).toHaveValue('Keep my draft');
    await page.locator('#sp-save-config').click();
    await expect(page.locator('#sp-page-message')).toContainText('设备配置已更新');
    expect((await smartState(page)).screenPanelConfig.pages[0].title).toBe('Changed on device');
  });

  test('WEB-PANEL-032: settings slider meets device minimum and custom size survives reload and text edits', async ({ authPage: page }, testInfo) => {
    await resetPanel(page); await openPanel(page); await generateThreeScreens(page);
    await selectComponent(page, 'system', 'brightness');
    await savePanel(page);
    const saved = (await smartState(page)).screenPanelConfig;
    const slider = saved.pages.flatMap((p: any) => p.cards).find((c: any) => c.visualType === 'slider');
    expect(slider.rect.w).toBeGreaterThanOrEqual(180);
    expect(slider.rect.h).toBeGreaterThanOrEqual(80);
    await page.locator('#sp-refresh-btn').click();
    await selectComponent(page, 'system', 'brightness');
    await expect(page.locator('#sp-card-size')).toHaveValue('custom');
    const original = { w: await page.locator('#sp-card-w').inputValue(), h: await page.locator('#sp-card-h').inputValue() };
    await page.locator('#sp-card-title').fill('Display brightness');
    await expect(page.locator('#sp-card-w')).toHaveValue(original.w);
    await expect(page.locator('#sp-card-h')).toHaveValue(original.h);
    await savePanel(page);
    await page.screenshot({ path: testInfo.outputPath('settings-slider-size.png'), fullPage: true });
    expect((await smartState(page)).requestCounts['POST /api/screen-panel/execute'] || 0).toBe(0);
  });

  test('WEB-PANEL-027: nested toolbar icons perform one draft action with undo and redo', async ({ authPage: page }) => {
    await resetPanel(page); await openPanel(page);
    // Exercise event delegation with nested icon markup, not synthetic button clicks.
    for (const selector of ['[data-sp-add="clock"]', '#sp-undo', '#sp-redo', '#sp-page-add']) {
      await page.locator(selector).evaluate(button => {
        const icon = document.createElement('span');
        icon.setAttribute('data-test-icon', 'true');
        icon.textContent = button.textContent;
        button.replaceChildren(icon);
      });
    }
    await page.locator('[data-sp-add="clock"] [data-test-icon]').click();
    await expect(page.locator('#sp-editor-canvas [data-kind="clock"]')).toHaveCount(1);
    await page.locator('#sp-undo [data-test-icon]').click();
    await expect(page.locator('#sp-editor-canvas [data-kind="clock"]')).toHaveCount(0);
    await page.locator('#sp-redo [data-test-icon]').click();
    await expect(page.locator('#sp-editor-canvas [data-kind="clock"]')).toHaveCount(1);
    await page.locator('#sp-page-add [data-test-icon]').click();
    await expect(page.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(2);
    await page.locator('#sp-undo [data-test-icon]').click();
    await expect(page.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(1);
    await expectDraftOnly(page);
  });

  test('WEB-PANEL-028: mobile property drawer remains clickable and preserves live geometry', async ({ authPage: page }, testInfo) => {
    await resetPanel(page); await openPanel(page);
    await page.locator('[data-sp-add="clock"]').click();
    for (const width of [390, 360]) {
      await page.setViewportSize({ width, height: 844 });
      if (await page.locator('#sp-properties-close').isVisible()) await page.locator('#sp-properties-close').click();
      await page.locator('#sp-component-properties').click();
      await page.locator('#sp-card-w').fill('220');
      await page.locator('#sp-card-h').fill('100');
      const rect = await page.locator('#sp-editor-canvas [data-kind="clock"]').evaluate(el => {
        const canvas = el.parentElement as HTMLElement;
        const canvasBox = canvas.getBoundingClientRect();
        const footerHeight = canvas.querySelector('.sp-device-footer')!.getBoundingClientRect().height;
        return { width: el.getBoundingClientRect().width, height: el.getBoundingClientRect().height,
          scaleX: canvasBox.width / Number(canvas.dataset.outputWidth),
          scaleY: (canvasBox.height - footerHeight) / Number(canvas.dataset.outputHeight) };
      });
      expect(rect.width).toBeCloseTo(220 * rect.scaleX, 1);
      expect(rect.height).toBeCloseTo(100 * rect.scaleY, 1);
      const close = page.locator('#sp-properties-close');
      await close.scrollIntoViewIfNeeded();
      await expect.poll(() => close.evaluate(el => {
        const box = el.getBoundingClientRect();
        return el.contains(document.elementFromPoint(box.x + box.width / 2, box.y + box.height / 2));
      })).toBe(true);
      await page.screenshot({ path: testInfo.outputPath('mobile-properties-' + width + '.png'), fullPage: true });
      await close.click();
      await expect(page.locator('#sp-card-w')).toBeHidden();
      await page.locator('#sp-global-settings').click();
      await expect(page.locator('#sp-global-properties')).toBeVisible();
      await close.click();
      await page.locator('#sp-component-properties').click();
      await expect(page.locator('#sp-card-w')).toHaveValue('220');
      await expect(page.locator('#sp-card-h')).toHaveValue('100');
      await close.click();
      await page.locator('#sp-save-config').scrollIntoViewIfNeeded();
      await expect(page.locator('#sp-save-config')).toBeInViewport();
      expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth + 1)).toBe(true);
      await expectDraftOnly(page);
    }
  });

  test('WEB-PANEL-029: rotated non-square canvas fits viewport without altering draft geometry', async ({ authPage: page }, testInfo) => {
    await resetPanel(page); await openPanel(page);
    const config = { schemaVersion: 1, displayRef: 'display_main', settings: { homePageId: 'home' }, pages: [{
      id: 'home', title: 'Rotated page', layoutMode: 'canvas', logicalWidth: 480, logicalHeight: 320, rotation: 90,
      cards: [{ id: 'clock', kind: 'clock', title: 'Clock', rect: { x: 24, y: 32, w: 160, h: 96 } }],
    }] };
    await page.locator('#sp-import-file').setInputFiles({ name: 'rotated.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify({ config })) });
    const baseline = await page.evaluate(() => JSON.stringify((window as any).AppState._spModel.config));
    for (const width of [1440, 1024, 390, 360]) {
      await page.setViewportSize({ width, height: 900 });
      if (await page.locator('#sp-properties-close').isVisible()) await page.locator('#sp-properties-close').click();
      await expect(page.locator('#sp-editor-canvas')).toHaveAttribute('data-output-width', '320');
      await expect(page.locator('#sp-editor-canvas')).toHaveAttribute('data-output-height', '480');
      const bounds = await page.evaluate(() => {
        const frame = document.querySelector('#sp-editor-frame')!.getBoundingClientRect().toJSON();
        const stage = document.querySelector('.sp-canvas-stage')!.getBoundingClientRect().toJSON();
        const card = document.querySelector('#sp-editor-canvas [data-card-id="clock"]')!.getBoundingClientRect().toJSON();
        return { frame, stage, card, overflow: document.documentElement.scrollWidth > innerWidth + 1 };
      });
      expect(bounds.overflow).toBe(false);
      expect(bounds.frame.x).toBeGreaterThanOrEqual(bounds.stage.x - 1);
      expect(bounds.frame.right).toBeLessThanOrEqual(bounds.stage.right + 1);
      expect(bounds.frame.bottom).toBeLessThanOrEqual(bounds.stage.bottom + 1);
      expect(bounds.card.x).toBeGreaterThanOrEqual(bounds.frame.x);
      expect(bounds.card.right).toBeLessThanOrEqual(bounds.frame.right + 1);
      expect(bounds.card.bottom).toBeLessThanOrEqual(bounds.frame.bottom + 1);
      expect(await page.evaluate(() => JSON.stringify((window as any).AppState._spModel.config))).toBe(baseline);
      await page.screenshot({ path: testInfo.outputPath('rotated-' + width + '.png'), fullPage: true });
    }
    await expectDraftOnly(page);
  });

  test('WEB-PANEL-025: compact workspace fits desktop and mobile without changing layout', async ({ authPage: page }, testInfo) => {
    await resetPanel(page); await openPanel(page); await generateThreeScreens(page); await savePanel(page);
    await page.reload(); await openPanel(page);
    const config = await page.evaluate(() => JSON.stringify((window as any).AppState._spModel.config));
    for (const [width, height] of [[1866, 1000], [1440, 900], [1366, 768], [1024, 768], [390, 844], [360, 800]]) {
      await page.setViewportSize({ width, height });
      if (await page.locator('#sp-properties-close').isVisible()) await page.locator('#sp-properties-close').click();
      await page.locator('#sp-properties-toggle').click();
      if (width <= 1200) await page.locator('#sp-properties-close').click();
      const sizes = await page.evaluate(() => {
        const rect = (selector: string) => document.querySelector(selector)!.getBoundingClientRect().toJSON();
        return { canvas: rect('#sp-editor-frame'), stage: rect('.sp-canvas-stage'), save: rect('.sp-save-bar'),
          content: rect('.content'), layout: rect('.sp-editor-layout'), toolbox: rect('.sp-toolbox'),
          overflow: document.documentElement.scrollWidth > innerWidth + 1 };
      });
      expect(sizes.overflow).toBeFalsy();
      expect(sizes.canvas.width).toBeGreaterThan(190);
      expect(sizes.canvas.x).toBeGreaterThanOrEqual(sizes.stage.x);
      expect(sizes.canvas.right).toBeLessThanOrEqual(sizes.stage.right + 1);
      if (width > 760) {
        expect(sizes.canvas.bottom).toBeLessThanOrEqual(sizes.stage.bottom + 1);
        expect(sizes.save.bottom).toBeLessThanOrEqual(sizes.content.bottom + 1);
        expect(sizes.toolbox.bottom).toBeLessThanOrEqual(sizes.layout.bottom + 1);
      }
      await page.screenshot({ path: testInfo.outputPath(`editor-${width}.png`), fullPage: true });
      expect(await page.evaluate(() => JSON.stringify((window as any).AppState._spModel.config))).toBe(config);
    }
    await page.setViewportSize({ width: 1440, height: 900 });
    await page.locator('[data-sp-section="sp-component-tools"]').click();
    await expect(page.locator('[data-sp-add="data"]')).toBeInViewport();
    await page.locator('#sp-editor-canvas [data-kind="data"]').first().click();
    await expect(page.locator('#sp-card-source')).toBeVisible();
    await page.screenshot({ path: testInfo.outputPath('component-properties.png'), fullPage: true });
    await page.locator('[data-sp-section="sp-page-tools"]').click();
    await expect(page.locator('#sp-page-tabs')).toBeInViewport();
    await page.locator('#sp-properties-toggle').click();
    await page.locator('#sidebar-toggle').click();
    await page.screenshot({ path: testInfo.outputPath('editor-focused.png'), fullPage: true, animations: 'disabled' });
  });

  test('WEB-PANEL-026: more actions close after selection and preserve property switching', async ({ authPage: page }) => {
    await resetPanel(page); await openPanel(page);
    for (const width of [1440, 390]) {
      await page.setViewportSize({ width, height: 900 });
      if (await page.locator('#sp-properties-close').isVisible()) await page.locator('#sp-properties-close').click();
      await page.locator('.sp-more summary').click();
      await expect(page.locator('#sp-more-actions')).toBeVisible();
      await page.locator('#sp-weather-settings').click();
      await expect(page.locator('#sp-weather-properties')).toBeVisible();
      await expect(page.locator('#sp-more-actions')).toBeHidden();
      if (width <= 1200) await page.locator('#sp-properties-close').click();
      await page.locator('#sp-global-settings').click();
      await expect(page.locator('#sp-global-properties')).toBeVisible();
      if (width <= 1200) await page.locator('#sp-properties-close').click();
      await page.locator('.sp-more summary').click();
      await page.keyboard.press('Escape');
      await expect(page.locator('#sp-more-actions')).toBeHidden();
    }
  });

  test('WEB-PANEL-024: unavailable touch explains controls and recovery reenables them', async ({ authPage }, testInfo) => {
    await resetPanel(authPage); await openPanel(authPage);
    await authPage.locator('#sp-global-settings').click();
    await expect(authPage.locator('[data-sp-nav="features"]')).toHaveCount(0);
    await expect(authPage.locator('#sp-apply-summary')).not.toContainText('设备已应用版本');
    const before = await authPage.evaluate(() => JSON.stringify((window as any).AppState._spModel.config));
    let recovered = false;
    await authPage.route('**/api/screen-panel/state*', async route => {
      const response = await route.fetch(); const body = await response.json();
      body.data.capabilities = { brightnessContinuous: recovered, buzzer: recovered, sleep: recovered, systemRestart: recovered,
        controlConfirmIdle: true, touchConfigured: true, touchReady: recovered,
        disabledReasons: recovered ? {} : { brightnessContinuous: '触摸控制器未就绪，正在重试', buzzer: '触摸控制器未就绪，正在重试', sleep: '触摸控制器未就绪，正在重试', systemRestart: '触摸控制器未就绪，正在重试' } };
      await route.fulfill({ response, json: body });
    });
    await authPage.locator('#sp-preview-refresh-btn').dispatchEvent('click');
    await expect(authPage.locator('[data-sp-add="buzzer"]')).toBeDisabled();
    await expect(authPage.locator('[data-sp-add="restart"]')).toBeDisabled();
    await expect(authPage.locator('#sp-system-capabilities')).toHaveText('触摸控制器未就绪，正在重试');
    for (const width of [1440, 390]) {
      await authPage.setViewportSize({ width, height: 900 });
      if (await authPage.locator('#sp-properties-close').isVisible()) {
        await authPage.locator('#sp-properties-close').click();
      }
      await authPage.locator('#sp-global-settings').click();
      await expect(authPage.locator('#sp-system-capabilities')).toBeVisible();
      await authPage.locator('#sp-system-capabilities').scrollIntoViewIfNeeded();
      expect(await authPage.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1)).toBeTruthy();
      await authPage.screenshot({ path: testInfo.outputPath(`global-properties-${width}.png`), fullPage: true });
    }
    recovered = true;
    await authPage.locator('#sp-preview-refresh-btn').dispatchEvent('click');
    await expect(authPage.locator('[data-sp-add="buzzer"]')).toBeEnabled();
    await expect(authPage.locator('[data-sp-add="restart"]')).toBeEnabled();
    await expect(authPage.locator('#sp-system-capabilities')).toBeHidden();
    expect(await authPage.evaluate(() => JSON.stringify((window as any).AppState._spModel.config))).toBe(before);
  });

  test('WEB-PANEL-020: live geometry, size presets and explicit property modes', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage);
    await authPage.locator('[data-sp-add="clock"]').click();
    await authPage.locator('#sp-card-size').selectOption('small');
    const card = authPage.locator('#sp-editor-canvas [data-kind="clock"]');
    const small = await card.boundingBox();
    await authPage.locator('#sp-card-size').selectOption('wide');
    expect((await card.boundingBox())!.width).toBeGreaterThan(small!.width + 50);
    await authPage.locator('#sp-card-w').fill('280');
    await authPage.locator('#sp-card-h').fill('120');
    await authPage.locator('#sp-card-x').fill('40');
    const rect = Object.fromEntries(await Promise.all(['x', 'y', 'w', 'h'].map(async key => [key, Number(await authPage.locator('#sp-card-' + key).inputValue())])));
    expect(rect).toMatchObject({ x: 40, w: 280, h: 120 });
    const rendered = await card.evaluate(el => ({ width: el.getBoundingClientRect().width,
      canvasWidth: el.parentElement!.getBoundingClientRect().width, logicalWidth: Number((el.parentElement as HTMLElement).dataset.outputWidth) }));
    expect(rendered.width).toBeCloseTo(280 * rendered.canvasWidth / rendered.logicalWidth, 1);
    await authPage.locator('#sp-properties-toggle').click();
    await expect(authPage.locator('#sp-page-properties')).toBeVisible();
    await expect(authPage.locator('#sp-inspector-form')).toBeHidden();
    await authPage.locator('#sp-component-properties').click();
    await expect(authPage.locator('#sp-card-w')).toHaveValue('280');
    await savePanel(authPage);
    await authPage.reload(); await openPanel(authPage);
    expect((await smartState(authPage)).screenPanelConfig.pages[0].cards[0].rect).toMatchObject(rect);
  });

  test('WEB-PANEL-021: free canvas additions preserve neighbors and cancelled drag rolls back', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage);
    await authPage.locator('[data-sp-add="clock"]').click();
    await authPage.locator('#sp-card-size').selectOption('small');
    const first = authPage.locator('#sp-editor-canvas [data-kind="clock"]');
    const before = await first.getAttribute('style');
    await authPage.locator('[data-sp-add="status"]').click();
    expect(await first.getAttribute('style')).toEqual(before);
    await first.evaluate(el => { el.setAttribute('data-original-node', 'true'); el.addEventListener('pointerdown', e => { (window as any).testPointerId = (e as PointerEvent).pointerId; }, { once: true }); });
    const box = (await first.boundingBox())!;
    await authPage.mouse.move(box.x + 30, box.y + 30); await authPage.mouse.down();
    await authPage.mouse.move(box.x + 46, box.y + 54, { steps: 4 });
    await authPage.waitForTimeout(8500);
    await expect(first).toHaveAttribute('data-original-node', 'true');
    await authPage.evaluate(() => document.dispatchEvent(new PointerEvent('pointerup', { pointerId: 999 })));
    await expect(authPage.locator('#sp-editor-canvas')).toHaveClass(/is-dragging/);
    await authPage.evaluate(() => document.dispatchEvent(new PointerEvent('pointercancel', { pointerId: (window as any).testPointerId })));
    await authPage.mouse.up();
    expect(await first.getAttribute('style')).toEqual(before);
  });

  test('WEB-PANEL-022: chart connects interleaved sources and preserves actual time gaps', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage);
    let points = [0, 1, 2, 3].map(i => ({ seq: 1 + i * 7, uptimeMs: 1000 + i * 1000, value: 10 + i, restored: false }));
    await authPage.route('**/api/periph-exec/history?*', route => route.fulfill({ json: { success: true, data: { points } } }));
    await generateThreeScreens(authPage);
    await authPage.locator('#sp-page-tabs [data-page-id="trend"]').click();
    const chart = authPage.locator('#sp-editor-canvas .sp-line-chart').first();
    await expect(chart.locator('polyline')).toHaveCount(3);
    await expect(chart.locator('circle')).toHaveCount(4);
    points[2].uptimeMs = 100000; points[3].uptimeMs = 101000;
    await authPage.locator('#sp-preview-refresh-btn').click();
    await expect(chart.locator('polyline')).toHaveCount(2);
    points = points.slice(0, 1);
    await authPage.locator('#sp-preview-refresh-btn').click();
    await expect(chart.locator('polyline')).toHaveCount(0);
    await expect(chart.locator('circle')).toHaveCount(1);
  });

  test('WEB-PANEL-023: typed selectable sources preserve missing bindings during unrelated edits', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage);
    await authPage.locator('[data-sp-add="gauge"]').click();
    const options = await authPage.locator('#sp-card-source option').evaluateAll(items => items.map(item => (item as HTMLOptionElement).value).filter(Boolean));
    expect(options.length).toBeGreaterThan(1);
    const choices = (await (await authPage.request.get('/api/screen-panel/choices?kind=all&pageSize=50')).json()).data.items;
    expect(options.every(ref => choices.some((c: any) => (c.ref || c.sourceRef || c.controlRef) === ref && (c.type || c.valueType) === 'number'))).toBe(true);
    await authPage.locator('#sp-card-source').selectOption(options[0]);
    await authPage.locator('#sp-card-source').selectOption(options[1]);
    await expect(authPage.locator('#sp-card-source')).toHaveValue(options[1]);
    const config = { schemaVersion: 1, displayRef: 'display_main', settings: { homePageId: 'home' }, pages: [{ id: 'home', title: '首页', layoutMode: 'canvas', logicalWidth: 480, logicalHeight: 480, cards: [{ id: 'missing', kind: 'data', visualType: 'gauge', sourceRef: 'missing.sensor', title: '未连接来源', rect: { x: 8, y: 8, w: 240, h: 140 } }] }] };
    await authPage.locator('#sp-import-file').setInputFiles({ name: 'missing.json', mimeType: 'application/json', buffer: Buffer.from(JSON.stringify({ config })) });
    await authPage.locator('#sp-editor-canvas [data-card-id="missing"]').click();
    await authPage.locator('#sp-card-title').fill('保留离线绑定');
    await expect(authPage.locator('#sp-card-source')).toHaveValue('missing.sensor');
    await authPage.locator('#sp-save-config').click();
    await expect(authPage.locator('#sp-page-message')).toContainText('来源已不可用');
    expect((await smartState(authPage)).requestCounts['POST /api/screen-panel/execute'] || 0).toBe(0);
  });

  test('WEB-PANEL-015: one workspace previews live drafts with one save', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage);
    await expect(authPage.locator('.sp-top-tabs')).toHaveCount(0);
    await expect(authPage.locator('.sp-property-tabs > button')).toHaveText(['页面属性', '组件属性', '全局属性']);
    await expect(authPage.locator('#sp-save-config')).toBeVisible();
    await expect(authPage.locator('#sp-preview-source, #sp-preview-pagination, #sp-card-apply, #sp-history-save')).toHaveCount(0);
    await generateThreeScreens(authPage);
    await expect(authPage.locator('#sp-editor-canvas')).toBeVisible();
    await expect(authPage.locator('#sp-layout-editor > summary')).toHaveCount(0);
    await expect(authPage.locator('#sp-editor-canvas .sp-weather-main')).toBeVisible();
    expect((await smartState(authPage)).screenPanelConfig.pages[0].cards).toHaveLength(0);
    await openEditor(authPage); await savePanel(authPage);
    await expect(authPage.locator('#sp-editor-canvas .sp-weather-main')).toBeVisible();
    await expect(authPage.locator('#sp-save-config')).toBeVisible();
  });

  test('WEB-PANEL-016: five footer entries and more menu reach every saved page', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage); await savePanel(authPage);
    const footer = authPage.locator('#sp-editor-canvas .sp-device-footer button');
    await expect(footer).toHaveCount(5);
    for (const id of ['home', 'trend', 'motor', 'controls', 'system']) {
      await authPage.locator(`#sp-editor-canvas [data-sp-editor-page="${id}"]`).click();
      await expect(authPage.locator(`#sp-editor-canvas [data-sp-editor-page="${id}"]`)).toHaveClass(/is-active/);
    }
    await openEditor(authPage); await authPage.locator('#sp-page-add').click(); await savePanel(authPage);
    await authPage.locator('#sp-editor-canvas [data-sp-editor-page="__more__"]').click();
    await expect(authPage.locator('#sp-editor-canvas .sp-nav-more button')).toHaveCount(6);
    await authPage.locator('#sp-editor-canvas .sp-nav-more button').last().click();
    await expect(authPage.locator('#sp-editor-canvas .sp-empty-surface')).toBeVisible();
    const counts = (await smartState(authPage)).requestCounts;
    expect(counts['POST /api/screen-panel/execute'] || 0).toBe(0);
  });

  test('WEB-PANEL-019: custom one and two page layouts survive save reload and home deletion', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await openEditor(authPage);
    await expect(authPage.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(1);
    await authPage.locator('[data-sp-add="clock"]').click();
    await authPage.locator('#sp-card-title').fill('自定义时钟');
    await savePanel(authPage);
    const originalId = (await smartState(authPage)).screenPanelConfig.pages[0].id;
    await authPage.reload(); await openPanel(authPage);
    await expect(authPage.locator('#sp-editor-canvas .sp-device-footer button')).toHaveCount(1);
    await expect(authPage.locator('#sp-editor-canvas .sp-card-title')).toContainText('自定义时钟');
    await openEditor(authPage);
    await authPage.locator('#sp-page-preset').selectOption('blank');
    await authPage.locator('#sp-page-add').click();
    await expect(authPage.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(2);
    await authPage.locator('#sp-page-home').click();
    await savePanel(authPage);
    const twoPages = (await smartState(authPage)).screenPanelConfig;
    const customId = twoPages.pages[1].id;
    expect(twoPages.pages.map((item: any) => item.id)).toEqual([originalId, customId]);
    expect(twoPages.settings.homePageId).toBe(customId);
    await authPage.reload(); await openPanel(authPage);
    await expect(authPage.locator('#sp-editor-canvas .sp-device-footer button')).toHaveCount(2);
    await expect(authPage.locator(`#sp-editor-canvas [data-sp-editor-page="${customId}"]`)).toHaveClass(/is-active/);
    await authPage.locator(`#sp-editor-canvas [data-sp-editor-page="${originalId}"]`).click();
    await expect(authPage.locator('#sp-editor-canvas .sp-card-title')).toContainText('自定义时钟');
    await openEditor(authPage);
    await authPage.locator(`#sp-page-tabs [data-page-id="${customId}"]`).click();
    await authPage.locator('#sp-page-delete').click();
    await savePanel(authPage);
    const singlePage = (await smartState(authPage)).screenPanelConfig;
    expect(singlePage.pages.map((item: any) => item.id)).toEqual([originalId]);
    expect(singlePage.settings.homePageId).toBe(originalId);
    expect(singlePage.settings.bottomNavPageIds).toEqual([originalId]);
    await authPage.reload(); await openPanel(authPage);
    await expect(authPage.locator('#sp-editor-canvas .sp-device-footer button')).toHaveCount(1);
    await expect(authPage.locator(`#sp-editor-canvas [data-sp-editor-page="${originalId}"]`)).toHaveClass(/is-active/);
    await openEditor(authPage); await authPage.locator('#sp-page-delete').click();
    await expect(authPage.locator('#sp-page-message')).toHaveText('至少保留一个页面');
    await expect(authPage.locator('#sp-page-tabs .sp-page-tab')).toHaveCount(1);
    expect((await smartState(authPage)).requestCounts['POST /api/screen-panel/execute'] || 0).toBe(0);
  });

  test('WEB-PANEL-013: inherited appearance and logical zero brightness save together', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage);
    await authPage.locator('#sp-global-settings').click();
    await authPage.locator('#sp-theme-id').selectOption('light');
    await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-theme', 'light');
    await authPage.locator('#sp-brightness').evaluate((input: HTMLInputElement)=>{input.value='0';input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('change',{bubbles:true}));});
    await authPage.locator('#sp-touch-beep').uncheck();
    await authPage.locator('#sp-confirm-idle').fill('120'); await authPage.locator('#sp-confirm-idle').dispatchEvent('change');
    await authPage.locator('#sp-idle-sleep').selectOption('0');
    await authPage.locator('#sp-properties-toggle').click();
    await authPage.locator('#sp-page-theme').selectOption('dark');
    await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-theme', 'dark');
    await authPage.locator('#sp-page-theme').selectOption('');
    await savePanel(authPage);
    expect((await smartState(authPage)).screenPanelConfig.settings).toMatchObject({brightnessPercent:0,touchBeepEnabled:false,controlConfirmIdleSeconds:120,idleSleepSeconds:0,themeId:'light'});
  });

  test('WEB-PANEL-008: library drag and immediate properties edit only the draft', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await openEditor(authPage);
    await authPage.locator('[data-sp-add="clock"]').scrollIntoViewIfNeeded();
    await authPage.locator('[data-sp-add="clock"]').dragTo(authPage.locator('#sp-editor-canvas'), { targetPosition: { x: 80, y: 80 } });
    await expect(authPage.locator('#sp-editor-canvas [data-kind="clock"]')).toHaveCount(1);
    await authPage.locator('#sp-card-title').fill('现场时钟');
    await expect(authPage.locator('#sp-editor-canvas')).toContainText('现场时钟');
    expect((await smartState(authPage)).requestCounts['POST /api/screen-panel/config'] || 0).toBe(0);
    await savePanel(authPage);
    expect((await smartState(authPage)).screenPanelConfig.pages[0].cards[0].title).toBe('现场时钟');
  });

  test('WEB-PANEL-002: rotated canvas inverse drag preserves safe wire data', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await openEditor(authPage);
    const config = {schemaVersion:1,displayRef:'display_main',settings:{homePageId:'home'},pages:[{id:'home',title:'旋转页',layoutId:'detail',layoutMode:'canvas',logicalWidth:480,logicalHeight:320,rotation:90,cards:[{id:'temp',kind:'data',visualType:'value',title:'温度',sourceRef:'modbus.boiler.temperature',iconId:'custom-icon',rect:{x:24,y:32,w:160,h:96}}]}]};
    await authPage.locator('#sp-import-file').setInputFiles({name:'layout.json',mimeType:'application/json',buffer:Buffer.from(JSON.stringify({config}))});
    await openEditor(authPage);
    const canvas = authPage.locator('#sp-editor-canvas');
    await expect(canvas).toHaveAttribute('data-output-width','320');
    await expect(canvas).toHaveAttribute('data-output-height','480');
    const card = canvas.locator('.sp-panel-card'); const box = await card.boundingBox();
    await authPage.mouse.move(box!.x+box!.width/2,box!.y+box!.height/2); await authPage.mouse.down();
    await authPage.mouse.move(box!.x+box!.width/2+48,box!.y+box!.height/2,{steps:6}); await authPage.mouse.up();
    await savePanel(authPage);
    const wire = (await smartState(authPage)).screenPanelConfig;
    expect(wire.pages[0]).toMatchObject({logicalWidth:480,logicalHeight:320,rotation:90});
    expect(wire.pages[0].cards[0].rect.y).toBeLessThan(32);
    expect(JSON.stringify(wire)).not.toMatch(/apiKey|password|secret/);
  });

  test('WEB-PANEL-003: shared history saves revisions and conflicts preserve the live draft', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage);
    await selectComponent(authPage,'trend','line');
    await authPage.locator('#sp-history-sources').selectOption(['modbus.boiler.temperature','can.motor.rpm']);
    const post = authPage.waitForRequest(r=>r.method()==='POST' && new URL(r.url()).pathname==='/api/periph-exec/history/config');
    await savePanel(authPage); expect((await post).postDataJSON()).toMatchObject({expectedRevision:0,enabled:true});
    expect((await smartState(authPage)).historyConfig.sourceAllowlist).toEqual(expect.arrayContaining(['modbus.boiler.temperature','can.motor.rpm','legacy.missing']));
    await authPage.locator('#sp-card-title').fill('保留这个草稿');
    await changeSavedPanel(authPage); await authPage.locator('#sp-save-config').click();
    await expect(authPage.locator('#sp-page-message')).toContainText('设备配置已更新');
    await expect(authPage.locator('#sp-dirty-state')).toHaveText('有未保存修改');
    await authPage.evaluate(()=>{Object.keys(localStorage).filter(key=>key.startsWith('fastbee.screenPanel.draft.')).forEach(key=>localStorage.removeItem(key));});
    await authPage.locator('#sp-refresh-btn').click();
    await expect(authPage.locator('#sp-page-tabs [data-page-id="trend"]')).toHaveClass(/is-active/);
    await expect(authPage.locator('#sp-card-title')).toHaveValue('保留这个草稿');
    await expect(authPage.locator('#sp-editor-canvas')).toContainText('保留这个草稿');
  });

  test('WEB-PANEL-004: one contextual Seniverse form preserves stored key and never stores draft secrets', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage);
    await selectComponent(authPage,'home','weather');
    await expect(authPage.locator('#sp-weather-key')).toHaveCount(1);
    await expect(authPage.locator('#sp-source-field')).toBeHidden();
    await expect(authPage.locator('#sp-weather-key')).toHaveAttribute('placeholder','已设置，留空保留');
    await authPage.locator('#sp-weather-location').fill('beijing');
    const post=authPage.waitForRequest(r=>r.method()==='POST'&&new URL(r.url()).pathname==='/api/screen-panel/weather');
    await savePanel(authPage); expect((await post).postDataJSON()).toMatchObject({apiKey:'',location:'beijing',expectedRevision:1});
    await authPage.locator('#sp-weather-key').fill('never-persist-this-key');
    await authPage.locator('#sp-card-title').fill('新天气名称'); await authPage.waitForTimeout(220);
    expect(await authPage.evaluate(()=>JSON.stringify(localStorage))).not.toContain('never-persist-this-key');
    await authPage.locator('#sp-refresh-btn').click();
    await selectComponent(authPage,'home','weather');
    await expect(authPage.locator('#sp-weather-key')).toHaveValue('never-persist-this-key');
  });

  test('WEB-PANEL-010: weather update uses saved configuration and respects manual cooldown', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage); await selectComponent(authPage,'home','weather');
    await authPage.locator('#sp-weather-refresh').click();
    await expect(authPage.locator('#sp-weather-status')).toContainText('26.5℃');
    await authPage.locator('#sp-weather-location').fill('beijing');
    await authPage.locator('#sp-weather-refresh').click();
    await expect(authPage.locator('#sp-page-message')).toContainText('请先保存天气配置');
    expect((await smartState(authPage)).requestCounts['POST /api/screen-panel/weather/refresh']).toBe(1);

  });

  test('WEB-PANEL-MOTOR-ROW: forward stop reverse share one row without executing commands', async ({ authPage }, testInfo) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage);
    await selectComponent(authPage, 'motor', 'motor');
    for (const width of [1440, 390, 360]) {
      await authPage.setViewportSize({ width, height: 900 });
      if (await authPage.locator('#sp-properties-close').isVisible()) await authPage.locator('#sp-properties-close').click();
      const row = authPage.locator('#sp-editor-canvas .sp-motor-actions');
      const buttons = row.locator('button');
      await expect(buttons).toHaveText(['正转', '停止', '反转']);
      for (let index = 0; index < 3; index++) await expect(buttons.nth(index)).toBeDisabled();
      await expect(buttons.nth(0)).toHaveClass('sp-motor-forward');
      await expect(buttons.nth(1)).toHaveClass('sp-motor-stop');
      await expect(buttons.nth(2)).toHaveClass('sp-motor-reverse');
      const colors = await buttons.evaluateAll(elements => elements.map(element => getComputedStyle(element).backgroundColor));
      expect(colors).toEqual(['rgb(24, 116, 70)', 'rgb(168, 56, 66)', 'rgb(32, 91, 174)']);
      const boxes = await buttons.evaluateAll(elements => elements.map(element => {
        const box = element.getBoundingClientRect();
        return { x: box.x, y: box.y, width: box.width, height: box.height, fits: element.scrollWidth <= element.clientWidth && element.scrollHeight <= element.clientHeight };
      }));
      expect(boxes.every(box => box.fits && box.width > 0 && box.height >= 31.5)).toBe(true);
      const parameterBox = await authPage.locator('#sp-editor-canvas .sp-motor-param').last().boundingBox();
      expect(parameterBox).not.toBeNull();
      expect(boxes[0].y).toBeGreaterThanOrEqual(parameterBox!.y + parameterBox!.height);
      for (let index = 1; index < boxes.length; index++) {
        expect(Math.abs(boxes[index].y - boxes[0].y)).toBeLessThan(1);
        expect(Math.abs(boxes[index].width - boxes[0].width)).toBeLessThan(1);
        expect(boxes[index].x).toBeGreaterThan(boxes[index-1].x + boxes[index-1].width);
      }
      await authPage.locator('#sp-editor-frame').screenshot({ path: testInfo.outputPath(`motor-row-${width}.png`) });
    }
    expect((await smartState(authPage)).requestCounts['POST /api/screen-panel/execute'] || 0).toBe(0);
  });

  test('WEB-PANEL-012: motor draft survives partial save and responsive editor keeps one action surface', async ({ authPage }, testInfo) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage);
    await selectComponent(authPage,'motor','motor'); await authPage.locator('#sp-motor-speed').fill('0'); await authPage.locator('#sp-motor-pulse').fill('999');
    await selectComponent(authPage,'home','weather'); await authPage.locator('#sp-weather-location').fill('beijing');
    await changeSavedPanel(authPage); await authPage.locator('#sp-save-config').click();
    await expect(authPage.locator('#sp-page-message')).toContainText(/天气.*已保存/);
    await expect(authPage.locator('#sp-dirty-state')).toHaveText('有未保存修改');
    await selectComponent(authPage,'motor','motor'); await expect(authPage.locator('#sp-motor-speed')).toHaveValue('0');
    await expect(authPage.locator('#sp-motor-pulse')).toHaveValue('999');
    for(const width of [1440,390,360]) {
      await authPage.setViewportSize({width,height:900});
      if(width<1050) {if(await authPage.locator('#sp-properties-close').isVisible()) await authPage.locator('#sp-properties-close').click();await authPage.locator('#sp-global-settings').click();await expect(authPage.locator('#sp-global-properties')).toBeVisible();await authPage.locator('#sp-properties-close').click();}
      expect(await authPage.evaluate(()=>document.documentElement.scrollWidth<=innerWidth+1)).toBe(true);
      await authPage.screenshot({path:testInfo.outputPath(`editor-${width}.png`),fullPage:true});
    }
    expect((await smartState(authPage)).requestCounts['POST /api/screen-panel/execute']||0).toBe(0);
  });
  test('WEB-PANEL-018: draft recovery requires matching device identity and revision', async ({ authPage }) => {
    await resetPanel(authPage); await openPanel(authPage); await openEditor(authPage);
    await authPage.locator('[data-sp-add="clock"]').click();
    await authPage.locator('#sp-card-title').fill('仅属于设备甲');
    await expect.poll(()=>authPage.evaluate(()=>Object.keys(localStorage).filter(key=>key.startsWith('fastbee.screenPanel.draft.')).length)).toBe(1);
    const record=await authPage.evaluate(()=>{const key=Object.keys(localStorage).find(key=>key.startsWith('fastbee.screenPanel.draft.'))!;return {key,payload:JSON.parse(localStorage.getItem(key)!)};});
    expect(record.key).toContain('02:00:00:00:00:01');
    expect(record.payload.deviceMac).toBe('02:00:00:00:00:01');
    let mac='02:00:00:00:00:02';
    await authPage.route('**/api/network/status*',route=>route.fulfill({json:{success:true,data:{macAddress:mac}}}));
    await authPage.locator('#sp-refresh-btn').click();
    await expect(authPage.locator('#sp-editor-canvas .sp-empty-surface')).toBeVisible();
    await expect(authPage.locator('#sp-draft-recovery')).toBeHidden();
    expect(await authPage.evaluate(key=>Boolean(localStorage.getItem(key)),record.key)).toBe(true);
    mac='02:00:00:00:00:01';
    await authPage.locator('#sp-refresh-btn').click();
    await expect(authPage.locator('#sp-draft-recovery')).toBeVisible();
    await expect(authPage.locator('#sp-editor-canvas .sp-empty-surface')).toBeVisible();
    await authPage.locator('#sp-draft-restore').click();
    await expect(authPage.locator('#sp-editor-canvas')).toContainText('仅属于设备甲');
    // An older host-only key is retained but never offered for recovery.
    await authPage.evaluate(record=>{localStorage.removeItem(record.key);const legacy={...record.payload};delete legacy.deviceMac;localStorage.setItem(record.key.replace('.02:00:00:00:00:01',''),JSON.stringify(legacy));},record);
    mac=''; await authPage.locator('#sp-refresh-btn').click();
    await expect(authPage.locator('#sp-editor-canvas .sp-empty-surface')).toBeVisible();
    await expect(authPage.locator('#sp-draft-recovery')).toBeHidden();
    mac='02:00:00:00:00:01'; await authPage.locator('#sp-refresh-btn').click();
    await expect(authPage.locator('#sp-draft-recovery')).toBeHidden();
    await expect(authPage.locator('#sp-editor-canvas .sp-empty-surface')).toBeVisible();
  });

  test('WEB-PANEL-017: unified editor has no toolbar overlap', async ({ authPage }, testInfo) => {
    await resetPanel(authPage); await openPanel(authPage); await generateThreeScreens(authPage); await savePanel(authPage);
    for(const width of [1440,390]) {
      await authPage.setViewportSize({width,height:900}); await openEditor(authPage);
      const bounds = await authPage.evaluate(()=>{const canvas=document.getElementById('sp-editor-frame')!.getBoundingClientRect(),save=document.querySelector('.sp-save-bar')!.getBoundingClientRect();return {canvasBottom:canvas.bottom,saveTop:save.top};});
      expect(bounds.saveTop).toBeGreaterThanOrEqual(bounds.canvasBottom);
      await authPage.locator('#sp-runtime-badge').scrollIntoViewIfNeeded();
      await authPage.screenshot({path:testInfo.outputPath(`normal-editor-${width}.png`),fullPage:true});
        await expect(authPage.locator('#sp-save-config')).toBeVisible();
      await authPage.screenshot({path:testInfo.outputPath(`normal-preview-${width}.png`),fullPage:true});
    }
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

});
