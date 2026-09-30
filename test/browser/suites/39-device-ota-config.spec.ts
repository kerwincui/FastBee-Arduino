import { test, expect } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';

const root = path.resolve(__dirname, '../../..');
const fragment = fs.readFileSync(path.join(root, 'web-src/pages/fragments/device-ota.html'), 'utf8');

for (const width of [1440, 390, 360]) {
  test('OTA settings load and save independently at ' + width + 'px', async ({ page }, testInfo) => {
    await page.setViewportSize({ width, height: 1000 });
    await page.setContent('<!doctype html><html lang="zh-CN"><meta charset="utf-8"><body><main style="max-width:1000px;margin:auto;padding:16px">' + fragment + '</main></body></html>');
    await page.addStyleTag({ path: path.join(root, 'web-src/css/main.css') });
    const pageErrors: string[] = [];
    page.on('pageerror', error => pageErrors.push(error.message));
    await page.evaluate(() => {
      const w = window as any;
      w.otaRequests = [];
      w.AppState = {
        registerModule(_name: string, methods: object) { Object.assign(this, methods); },
        onPageHide() {},
      };
      w.apiGet = w.apiGetFresh = async (url: string) => {
        if (url.includes('capabilities')) return { success: true, data: { ota: true } };
        if (url.includes('section=ota')) return { success: true, data: { otaBaseUrl: 'https://platform.example.com/releases' } };
        if (url.includes('network/status')) return { success: true, data: { connected: true } };
        if (url.includes('system/info')) return { success: true, data: { device: { firmwareVersion: 'test' }, flash: { total: 16777216, freeSketchSpace: 4194304 } } };
        return { success: true, data: { status: 'idle', in_progress: false, progress: 0 } };
      };
      w.apiPost = async (url: string, body: object) => {
        w.otaRequests.push({ url, body });
        return { success: true };
      };
    });
    await page.addScriptTag({ path: path.join(root, 'web-src/modules/runtime/device-ota.js') });
    await page.evaluate(() => (window as any).AppState.setupDeviceOta());
    const input = page.locator('#ota-base-url');
    await expect(input).toHaveValue('https://platform.example.com/releases');
    await expect(input).toBeEnabled();
    await input.fill('http://192.168.5.2:8080/firmware');
    await page.locator('#ota-config-save-btn').click();
    await expect(page.locator('#ota-config-message')).toContainText('无需重启');
    expect(await page.evaluate(() => (window as any).otaRequests)).toEqual([
      { url: '/api/protocol/config', body: { mqtt_otaBaseUrl: 'http://192.168.5.2:8080/firmware' } },
    ]);
    await input.fill('');
    await page.locator('#ota-config-save-btn').click();
    expect(await page.evaluate(() => (window as any).otaRequests.at(-1).body)).toEqual({ mqtt_otaBaseUrl: '' });
    await input.fill('https://platform.example.com?unexpected=1');
    await page.locator('#ota-config-save-btn').click();
    await expect(page.locator('#ota-config-message')).toContainText('请输入有效');
    expect(await page.evaluate(() => (window as any).otaRequests.length)).toBe(2);
    await input.fill('https://platform.example.com/releases');
    await page.locator('#ota-config-save-btn').click();
    expect(pageErrors).toEqual([]);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(true);
    await page.screenshot({ path: testInfo.outputPath('ota-settings-' + width + '.png'), fullPage: true });
  });
}
