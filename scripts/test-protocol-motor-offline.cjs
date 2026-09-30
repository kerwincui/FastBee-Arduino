'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { gunzipSync } = require('node:zlib');
const { chromium } = require('../test/browser/node_modules/playwright');
const root = path.resolve(__dirname, '..');
const cssFile = process.argv[2] || path.join(root, 'web-src/css/main.css');
const data = fs.readFileSync(cssFile);
const css = (cssFile.endsWith('.gz') ? gunzipSync(data) : data).toString('utf8');
async function main() {
    const browser = await chromium.launch({ headless: true });
    try {
        for (const width of [1280, 390]) {
            const page = await browser.newPage({ viewport: { width, height: 844 } });
            await page.setContent('<style>' + css + '</style><div id="modbus-device-ctrl-modal" class="modal" style="display:block;opacity:1"><div class="modal-content"><div class="modal-body"><button data-action="refreshMotorStatus">Refresh</button><div class="motor-actions"><button data-action="motorForward">Forward</button><button data-action="motorStop">Stop</button><button data-action="motorReverse">Reverse</button></div></div></div></div>');
            await page.evaluate(() => { window.AppState = {}; window.clicked = []; });
            await page.addScriptTag({ path: path.join(root, 'web-src/modules/runtime/protocol/modbus-control.js') });
            await page.evaluate(() => {
                AppState._setCtrlModalOffline(true);
                document.querySelectorAll('button').forEach(button => button.addEventListener('click', () => window.clicked.push(button.dataset.action)));
            });
            assert.equal(await page.locator('[data-action="motorForward"]').isDisabled(), true);
            assert.equal(await page.locator('[data-action="motorReverse"]').isDisabled(), true);
            await page.locator('[data-action="motorStop"]').click({ timeout: 3000 });
            await page.locator('[data-action="refreshMotorStatus"]').click({ timeout: 3000 });
            assert.deepEqual(await page.evaluate(() => window.clicked), ['motorStop', 'refreshMotorStatus']);
            await page.evaluate(() => AppState._markCtrlModalOnline());
            assert.equal(await page.locator('[data-action="motorForward"]').isDisabled(), false);
            await page.close();
        }
        console.log('Offline motor controls passed at desktop/mobile widths: stop and refresh clickable; movement disabled.');
    } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
