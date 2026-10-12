import {test, expect} from '@playwright/test';
import {resolve} from 'node:path';
const root = resolve(__dirname, '../../..');
const modules = {core: 'device-control/core.js', rules: 'periph-exec.js'};

for (const first of ['core', 'rules'] as const) {
  test(`shared value dialog survives ${first} first and the other lazy page loading`, async ({page}) => {
    const errors: string[] = [], requests: string[] = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.route('**/*', route => {requests.push(route.request().url()); return route.abort();});
    await page.setContent('<!doctype html><meta charset="utf-8"><main></main>');
    await page.evaluate(() => {
      const w = window as any;
      w.__results = [];
      w.Notification = {warning() {}};
      w.AppState = {registerModule(_name: string, methods: object) {Object.assign(this, methods);},
        showElement(el: HTMLElement) {el.hidden = false;}, hideElement(el: HTMLElement) {el.hidden = true;},
        showModal(el: HTMLElement) {el.hidden = false;},
        hideModal(id: string) {document.getElementById(id)!.hidden = true;}};
    });
    await page.addScriptTag({path: resolve(root, 'web-src/js/utils.js')});
    const load = (name: 'core' | 'rules') => page.addScriptTag({path: resolve(root, 'web-src/modules/runtime', modules[name])});
    await load(first);
    await page.evaluate(() => {const w = window as any; w.AppState.promptPeriphExecRunValue({defaultValue: 0}).then((value: any) => w.__results.push(value));});
    await expect(page.locator('#periph-exec-run-value-input')).toHaveValue('0');
    await load(first === 'core' ? 'rules' : 'core');
    await page.locator('#periph-exec-run-value-input').fill(' ');
    await page.locator('#confirm-periph-exec-run-value-btn').click();
    expect(await page.evaluate(() => (window as any).__results)).toEqual([]);
    await expect(page.locator('#periph-exec-run-value-error')).toContainText('请输入');
    await page.locator('#periph-exec-run-value-input').fill('0');
    await page.locator('#periph-exec-run-value-input').press('Enter');
    expect(await page.evaluate(() => (window as any).__results)).toEqual(['0']);
    await page.evaluate(() => {const w = window as any; w.AppState.promptPeriphExecRunValue({defaultValue: '1'}).then((value: any) => w.__results.push(value));});
    await page.locator('#cancel-periph-exec-run-value-btn').click();
    expect(await page.evaluate(() => (window as any).__results)).toEqual(['0', null]);
    await expect(page.locator('#periph-exec-run-value-modal')).toHaveCount(1);
    expect(requests).toEqual([]);
    expect(errors).toEqual([]);
  });
}
