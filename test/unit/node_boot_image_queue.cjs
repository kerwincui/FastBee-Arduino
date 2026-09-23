const assert = require('node:assert/strict');
const fs = require('node:fs');
const { chromium } = require('../browser/node_modules/playwright');
const { minifyJS } = require('../../scripts/minify-js');
const original = fs.readFileSync('web-src/index.html', 'utf8');
// A valid tiny PNG is sufficient to verify browser image loading and blob reuse.
const png = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+a2ioAAAAASUVORK5CYII=', 'base64');
(async () => {
  const browser = await chromium.launch({headless:true});
  try {
    for (const minified of [false, true]) {
      const page = await browser.newPage();
      const calls=[]; let active=0,maxActive=0;
      const html = minified ? original.replace(/<script>([\s\S]*?)<\/script>/g, (_, code) => '<script>'+minifyJS(code)+'</script>') : original;
      await page.route('http://fastbee.test/**', async route => {
        const pathname = new URL(route.request().url()).pathname;
        if(pathname==='/') return route.fulfill({status:200,contentType:'text/html',body:html});
        calls.push(pathname); ++active;maxActive=Math.max(maxActive,active);
        await new Promise(resolve=>setTimeout(resolve,35));
        await route.fulfill({status:200,contentType:pathname.startsWith('/assets/')?'image/png':'application/javascript',body:pathname.startsWith('/assets/')?png:'/* boot fixture */'});
        --active;
      });
      await page.goto('http://fastbee.test/',{waitUntil:'domcontentloaded'});
      await page.waitForFunction(()=>document.querySelectorAll('script[src]').length===3&&document.getElementById('login-logo').complete&&document.getElementById('login-logo').naturalWidth>0);
      await page.evaluate(()=>window.__fastbeeStaticAssetChain);
      assert.equal(maxActive,1,'image/icon must not overlap boot scripts');
      assert.equal(calls.filter(p=>p==='/assets/logo.png').length,1);
      assert.equal(calls.filter(p=>p==='/assets/favicon.ico').length,1);
      await page.evaluate(()=>window.__fastbeeLoadImageAsset(document.getElementById('sidebar-logo'),'/assets/logo.png'));
      await page.waitForTimeout(80);
      assert.equal(calls.filter(p=>p==='/assets/logo.png').length,1,'sidebar uses completed logo blob without another HTTP request');
      assert.equal(await page.locator('#sidebar-logo').getAttribute('src'),await page.locator('#login-logo').getAttribute('src'));
      await page.close();
    }
  } finally {await browser.close();}
  console.log('real browser logo/favicon/boot serialization and sidebar blob reuse passed');
})().catch(error=>{console.error(error);process.exitCode=1;});
