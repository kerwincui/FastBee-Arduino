// 调试: 轮询触发模式(triggerType=5)下动作块行为
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

  const dump = async (tag) => {
    const s = await page.evaluate(() => {
      const ablock = document.querySelector('#periph-exec-actions .periph-exec-config-item');
      const tblock = document.querySelector('#periph-exec-triggers .periph-exec-config-item');
      const el = (b, sel) => { const e = b?.querySelector(sel); if (!e) return null; return { cls: e.className, disp: getComputedStyle(e).display, val: e.value }; };
      return {
        triggerType: el(tblock, '.pe-trigger-type'),
        actionType: el(ablock, '.pe-action-type'),
        actionTypeOptions: Array.from(ablock?.querySelector('.pe-action-type')?.querySelectorAll('option') || []).map(o => o.value),
        pollTasks: el(ablock, '.pe-poll-tasks-group'),
        targetGroup: el(ablock, '.pe-target-group'),
        sensorGroup: el(ablock, '.pe-sensor-group'),
      };
    });
    console.log('=== ' + tag + ' ===');
    console.log(JSON.stringify(s, null, 1));
  };

  await dump('初始(平台触发)');

  // 切换触发类型为轮询触发(5)
  await page.locator('#periph-exec-triggers .periph-exec-config-item').first().locator('.pe-trigger-type').selectOption('5');
  await page.waitForTimeout(1000);
  await dump('切换为轮询触发(5)后');

  await browser.close();
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
