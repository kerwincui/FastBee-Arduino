// 调试 EXEC-110: 实际观察 OLED(27) 选择后的 DOM 显隐状态
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

  // 导航到外设执行页
  await page.click('.menu-item[data-page="periph-exec"]', { timeout: 10000 });
  await page.waitForSelector('#periph-exec-page', { state: 'visible', timeout: 15000 });
  await page.waitForTimeout(1500);

  // 打开新增弹窗
  await page.click('#periph-exec-page-add-btn');
  await page.waitForSelector('#periph-exec-modal', { state: 'visible', timeout: 10000 });
  await page.waitForTimeout(2000);

  const dump = async (tag) => {
    const state = await page.evaluate(() => {
      const block = document.querySelector('#periph-exec-actions .periph-exec-config-item');
      if (!block) return { error: 'no block' };
      const get = (sel) => {
        const el = block.querySelector(sel);
        if (!el) return null;
        const cs = getComputedStyle(el);
        return {
          tag: el.tagName,
          cls: el.className,
          display: cs.display,
          visibility: cs.visibility,
          hasIsHidden: el.classList.contains('is-hidden'),
          rect: el.getBoundingClientRect().height,
        };
      };
      return {
        actionType: block.querySelector('.pe-action-type')?.value,
        single: get('.pe-action-value'),
        oled: get('.pe-action-value-oled'),
        script: get('.pe-action-value-script'),
        valueGroup: get('.pe-action-value-group'),
        targetGroup: get('.pe-target-group'),
      };
    });
    console.log('=== ' + tag + ' ===');
    console.log(JSON.stringify(state, null, 2));
  };

  await dump('初始状态(默认动作)');

  // 选择 OLED (27)
  await block(page).locator('.pe-action-type').selectOption('27');
  await page.waitForTimeout(800);
  await dump('选择OLED(27)后');

  // 选择 闪烁 (2)
  await block(page).locator('.pe-action-type').selectOption('2');
  await page.waitForTimeout(800);
  await dump('选择闪烁(2)后');

  await browser.close();
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });

function block(page) {
  return page.locator('#periph-exec-actions .periph-exec-config-item').first();
}
