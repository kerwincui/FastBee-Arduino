// 诊断: 用 MutationObserver 追踪 singleEl 的 class 变化
const { chromium } = require('playwright');
const IP = process.env.DEVICE_IP || '192.168.5.18';

(async () => {
  const browser = await chromium.launch();
  const page = await browser.newPage();
  page.on('dialog', d => d.accept().catch(() => {}));
  page.on('console', m => { if (m.type() === 'error') console.log('[console.error]', m.text()); });
  page.on('pageerror', e => console.log('[pageerror]', e.message));

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

  // 同步追踪 singleEl classList.add/remove 调用序列
  await page.evaluate(() => {
    const block = document.querySelector('#periph-exec-actions .periph-exec-config-item');
    const single = block.querySelector('.pe-action-value');
    window.__syncLog = [];
    const cl = single.classList;
    const origAdd = cl.add.bind(cl);
    const origRemove = cl.remove.bind(cl);
    cl.add = (...c) => { window.__syncLog.push('ADD:' + c.join(',') + ' => ' + single.className); const r = origAdd(...c); window.__syncLog[window.__syncLog.length-1] += ' | after=' + single.className; try { window.__syncLog.push('  stack=' + new Error().stack.split('\n').slice(2,5).join(' << ')); } catch(e){} return r; };
    cl.remove = (...c) => { window.__syncLog.push('REM:' + c.join(',') + ' => ' + single.className); const r = origRemove(...c); window.__syncLog[window.__syncLog.length-1] += ' | after=' + single.className; try { window.__syncLog.push('  stack=' + new Error().stack.split('\n').slice(2,5).join(' << ')); } catch(e){} return r; };
  });

  await page.locator('#periph-exec-actions .periph-exec-config-item').first().locator('.pe-action-type').selectOption('27');
  await page.waitForTimeout(1000);

  const result = await page.evaluate(() => {
    const block = document.querySelector('#periph-exec-actions .periph-exec-config-item');
    const single = block.querySelector('.pe-action-value');
    const oled = block.querySelector('.pe-action-value-oled');
    return {
      syncLog: window.__syncLog,
      singleFinal: single.className,
      oledFinal: oled.className,
      actionType: block.querySelector('.pe-action-type').value,
    };
  });
  console.log(JSON.stringify(result, null, 2));
  await browser.close();
})().catch(e => { console.error('FATAL:', e.message); process.exit(1); });
