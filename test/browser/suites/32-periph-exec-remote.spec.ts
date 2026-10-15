import { test, expect } from '../fixtures/base.fixture';
import { uiScreenshotPath } from '../evidence-paths';
const { MAPPING: privateNames } = require('../../../scripts/web-private-properties');

test.describe('PeriphExec weather and retired remote actions', () => {
  test('WEB-PANEL-005: new rules offer only current weather and serialize action 34', async ({ authPage, navigateTo }) => {
    expect((await authPage.request.post('/api/test/smart-panel-reset')).ok()).toBeTruthy();
    await navigateTo('periph-exec');
    await authPage.locator('#periph-exec-page-add-btn').click();
    const modal = authPage.locator('#periph-exec-modal');
    await expect(modal).toBeVisible();
    await modal.locator('#periph-exec-name').fill('未保存的采集规则');
    await expect(modal.locator('#periph-exec-enabled')).toBeChecked();
    await expect(modal.locator('#periph-exec-report')).toBeChecked();
    await expect(modal.locator('#periph-exec-panel-control')).not.toBeChecked();
    await expect(modal.locator('#periph-exec-record-history')).not.toBeChecked();
    for (const width of [1280, 390]) {
      await authPage.setViewportSize({ width, height: 844 });
      expect(await modal.evaluate(node => node.scrollWidth <= node.clientWidth + 1)).toBeTruthy();
      await modal.screenshot({ path: uiScreenshotPath(`periph-rule-options-${width}.png`) });
    }
    await authPage.evaluate(names => {
      const app = (window as any).AppState;
      const key = names._updatePeriphExecPanelOptions in app ? names._updatePeriphExecPanelOptions : '_updatePeriphExecPanelOptions';
      app._featureActive.screenPanel = true; app[key]();
    }, privateNames);
    const popupPromise = authPage.waitForEvent('popup');
    await modal.locator('#periph-exec-history-settings-link').click();
    const popup = await popupPromise;
    await popup.waitForLoadState('domcontentloaded');
    await expect(modal).toBeVisible();
    await expect(modal.locator('#periph-exec-name')).toHaveValue('未保存的采集规则');
    await popup.close();
    const block = modal.locator('#periph-exec-actions .periph-exec-config-item').first();
    const actionType = block.locator('.pe-action-type');
    await expect(actionType.locator('option[value="33"]')).toHaveCount(0);
    await expect(actionType.locator('option[value="34"]')).toBeAttached();
    await actionType.selectOption('34');
    const target = block.locator('.pe-target-periph');
    await expect(target.locator('option[value="main"]')).toContainText('心知天气');
    await expect(target.locator('option[value="backup"]')).toHaveCount(0);
    await target.selectOption('main');
    await expect(block.locator('.pe-remote-value-group')).toBeHidden();
    await modal.locator('#periph-exec-name').fill('Current weather contract');
    const savedResponse = authPage.waitForResponse(response => response.url().endsWith('/api/periph-exec') && response.request().method() === 'POST');
    await modal.locator('#save-periph-exec-btn').click();
    expect((await savedResponse).ok()).toBeTruthy();
    const savedRules = await (await authPage.request.get('/api/periph-exec')).json();
    expect(savedRules.data.find((rule: any) => rule.name === 'Current weather contract').actions).toEqual([
      expect.objectContaining({ actionType: 34, targetPeriphId: 'main', actionValue: '', useReceivedValue: false }),
    ]);
  });

  test('WEB-PANEL-006: retired channel rules stay visible but cannot be saved as active actions', async ({ authPage, navigateTo }) => {
    expect((await authPage.request.post('/api/test/smart-panel-reset')).ok()).toBeTruthy();
    await navigateTo('periph-exec');
    await authPage.locator('#periph-exec-page-add-btn').click();
    const modal = authPage.locator('#periph-exec-modal');
    await expect(modal).toBeVisible();
    await expect(modal.locator('#periph-exec-actions')).toBeVisible();
    await authPage.evaluate(names => {
      const app = (window as any).AppState;
      const key = names._createPeriphExecActionElement in app ? names._createPeriphExecActionElement : '_createPeriphExecActionElement';
      document.getElementById('periph-exec-actions')!.innerHTML = '';
      app[key]({ actionType: 33, targetPeriphId: 'old-channel', actionValue: 'on' }, 0);
    }, privateNames);
    const block = modal.locator('#periph-exec-actions .periph-exec-config-item').first();
    await expect(block.locator('.pe-action-type option[value="33"]')).toBeDisabled();
    await expect(block.locator('.pe-action-type')).toContainText('远程通道已停用');
    await modal.locator('#periph-exec-name').fill('Retired action');
    await modal.locator('#save-periph-exec-btn').click();
    await expect(modal.locator('#periph-exec-error')).toContainText('远程通道已停用');
    const state = await (await authPage.request.get('/api/test/smart-panel-state')).json();
    expect(state.data.requestCounts['POST /api/periph-exec'] || 0).toBe(0);
  });
});
