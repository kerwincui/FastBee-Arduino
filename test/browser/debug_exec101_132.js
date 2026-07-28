// 调试: EXEC-101 设备传感器校准组显隐 + EXEC-132 重建后事件选择
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
  const cls = (sel) => page.evaluate((s) => document.querySelector('#periph-exec-actions .periph-exec-config-item ' + s)?.className ?? 'ABSENT', sel);

  // ---- EXEC-101 ----
  await ablock().locator('.pe-action-type').selectOption('19');
  await page.waitForTimeout(500);
  console.log('EXEC101 [select 19] calGroup cls = ' + await cls('.pe-sensor-calibration-group'));
  console.log('EXEC101 [select 19] advGroup cls = ' + await cls('.pe-sensor-advanced-group'));
  console.log('EXEC101 [select 19] category = ' + await ablock().locator('.pe-sensor-category').inputValue());
  // 切换类别到 dht11 再切回 analog
  await ablock().locator('.pe-sensor-category').selectOption('dht11');
  await page.waitForTimeout(400);
  console.log('EXEC101 [cat=dht11] calGroup cls = ' + await cls('.pe-sensor-calibration-group'));
  await ablock().locator('.pe-sensor-category').selectOption('analog');
  await page.waitForTimeout(400);
  console.log('EXEC101 [cat=analog] calGroup cls = ' + await cls('.pe-sensor-calibration-group'));
  console.log('EXEC101 [cat=analog] advGroup cls = ' + await cls('.pe-sensor-advanced-group'));

  // ---- EXEC-132 ----
  await ablock().locator('.pe-action-type').selectOption('21');
  await page.waitForTimeout(500);
  await ablock().locator('.pe-trigger-event-select').selectOption('wifi_connected');
  await page.waitForTimeout(300);
  console.log('EXEC132 [select 21+wifi] eventSelect cls = ' + await cls('.pe-trigger-event-select'));
  // 切换触发类型 1 -> 0 (触发重建)
  const tblock = page.locator('#periph-exec-triggers .periph-exec-config-item').first();
  await tblock.locator('.pe-trigger-type').selectOption('1');
  await page.waitForTimeout(600);
  await tblock.locator('.pe-trigger-type').selectOption('0');
  await page.waitForTimeout(600);
  console.log('EXEC132 [rebuild后] actionType = ' + await ablock().locator('.pe-action-type').inputValue());
  console.log('EXEC132 [rebuild后] eventSelect cls = ' + await cls('.pe-trigger-event-select'));
  console.log('EXEC132 [rebuild后] eventSelect value = ' + await ablock().locator('.pe-trigger-event-select').inputValue().catch(() => 'ABSENT'));

  await browser.close();
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
