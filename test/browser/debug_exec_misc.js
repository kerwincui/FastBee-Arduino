// 调试: EXEC-125 路径 + EXEC-101 + EXEC-132 的设备实际行为
const { chromium } = require('playwright');
const IP = process.env.DEVICE_IP || '192.168.5.18';

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();
  page.on('dialog', d => d.accept().catch(() => {}));

  await page.goto(`http://${IP}/`, { timeout: 30000 });
  await page.waitForSelector('#login-page', { state: 'visible', timeout: 30000 });
  await page.fill('#username', 'admin');
  await page.fill('#password', 'admin123');
  await page.click('#login-button');
  await page.waitForSelector('#app-container', { state: 'visible', timeout: 40000 });
  await page.waitForTimeout(2000);
  await page.click('.menu-item[data-page="periph-exec"]', { timeout: 10000 });
  await page.waitForSelector('#periph-exec-page', { state: 'visible', timeout: 15000 });
  await page.waitForTimeout(1500);
  await page.click('#periph-exec-page-add-btn');
  await page.waitForSelector('#periph-exec-modal', { state: 'visible', timeout: 10000 });
  await page.waitForTimeout(2000);

  const ablock = () => page.locator('#periph-exec-actions .periph-exec-config-item').first();

  const snap = async (tag) => {
    const s = await page.evaluate(() => {
      const b = document.querySelector('#periph-exec-actions .periph-exec-config-item');
      const vis = (sel) => { const e = b?.querySelector(sel); if (!e) return 'ABSENT'; return getComputedStyle(e).display !== 'none' ? 'VISIBLE' : 'hidden'; };
      const val = (sel) => b?.querySelector(sel)?.value ?? 'ABSENT';
      const txt = (sel) => b?.querySelector(sel)?.textContent?.trim() ?? 'ABSENT';
      return {
        actionType: val('.pe-action-type'),
        pollTasks: vis('.pe-poll-tasks-group'),
        targetGroup: vis('.pe-target-group'),
        targetLabel: txt('.pe-target-group label'),
        sensorGroup: vis('.pe-sensor-group'),
        valueGroup: vis('.pe-action-value-group'),
        oled: vis('.pe-action-value-oled'),
        sensorCategory: val('.pe-sensor-category'),
        calGroup: vis('.pe-sensor-calibration-group'),
        advGroup: vis('.pe-sensor-advanced-group'),
        eventSelect: vis('.pe-trigger-event-select'),
      };
    });
    console.log('=== ' + tag + ' === ' + JSON.stringify(s));
  };

  // ---- EXEC-125 路径 ----
  for (const at of ['0', '4', '19', '22', '27']) {
    await ablock().locator('.pe-action-type').selectOption(at);
    await page.waitForTimeout(400);
    await snap('EXEC125 actionType=' + at);
  }

  // ---- EXEC-101: 传感器读取 ----
  await ablock().locator('.pe-action-type').selectOption('19');
  await page.waitForTimeout(500);
  await snap('EXEC101 select 19');

  // ---- EXEC-132: 触发设备事件 ----
  await ablock().locator('.pe-action-type').selectOption('21');
  await page.waitForTimeout(500);
  await snap('EXEC132 select 21 (eventSelect应可见)');
  // 尝试选择 wifi_connected
  try {
    await ablock().locator('.pe-trigger-event-select').selectOption('wifi_connected', { timeout: 3000 });
    console.log('EXEC132 selectOption wifi_connected: OK');
  } catch (e) {
    console.log('EXEC132 selectOption wifi_connected FAILED: ' + e.message.split('\n')[0]);
    const opts = await page.evaluate(() => Array.from(document.querySelector('#periph-exec-actions .periph-exec-config-item .pe-trigger-event-select')?.querySelectorAll('option') || []).map(o => o.value));
    console.log('EXEC132 event options: ' + JSON.stringify(opts));
  }
  await snap('EXEC132 after select wifi_connected');

  await browser.close();
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
