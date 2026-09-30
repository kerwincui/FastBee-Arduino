import { env, expect, test } from '../fixtures/base.fixture';
import type { Page } from '@playwright/test';

async function open(page: Page) {
  expect(['127.0.0.1', '127.0.0.2', 'localhost']).toContain(new URL(env.baseURL).hostname);
  expect((await page.request.post('/api/test/smart-panel-reset')).ok()).toBeTruthy();
  await page.evaluate(() => Object.keys(localStorage).filter(k => k.startsWith('fastbee.screenPanel.draft.')).forEach(k => localStorage.removeItem(k)));
  await page.waitForFunction(() => Boolean((window as any).AppState?._featureStatusReady));
  await page.evaluate(async () => (window as any).AppState.changePage('screen-panel'));
  await page.waitForFunction(() => (window as any).AppState?._spModel?.loaded);
}

async function save(page: Page) {
  await page.locator('#sp-save-config').click();
  await expect(page.locator('#sp-dirty-state')).toHaveText('已保存');
}

test('WEB-PANEL-035: fresh loading preserves the saved page and component selection', async ({authPage: page}) => {
  await open(page); await page.locator('#sp-generate-three-screen').click(); await save(page);
  await page.locator('#sp-page-tabs [data-page-id="system"]').click();
  await page.locator('#sp-editor-canvas [data-kind="brightness"]').click();
  const selected = () => page.evaluate(() => ({
    page: document.querySelector('#sp-page-tabs [aria-selected="true"]')?.getAttribute('data-page-id'),
    card: document.querySelector('#sp-editor-canvas .sp-panel-card.is-selected')?.getAttribute('data-card-id'),
  }));
  const before = await selected();
  expect(before.page).toBe('system');
  expect(before.card).toBeTruthy();
  await page.evaluate(async () => (window as any).AppState.loadScreenPanelPage({noCache:true,preserveDraft:true}));
  expect(await selected()).toEqual(before);
  await expect(page.locator('#sp-card-title')).toBeVisible();
});

test('WEB-PANEL-036: starter layouts exclude unsupported hardware actions', async ({authPage: page}) => {
  await open(page);
  await page.route('**/api/screen-panel/state*', async route => {
    const response = await route.fetch(), body = await response.json();
    body.data.capabilities = {...body.data.capabilities,brightnessContinuous:false,buzzer:false};
    await route.fulfill({json:body});
  });
  await page.evaluate(async () => (window as any).AppState.loadScreenPanelPage({noCache:true,preserveDraft:true}));
  await page.locator('#sp-generate-three-screen').click();
  expect(await page.evaluate(() => (window as any).AppState._spModel.config.pages.flatMap((p:any)=>p.cards).filter((c:any)=>['brightness','buzzer','touch-beep'].includes(c.kind)))).toEqual([]);
  await save(page);
});
