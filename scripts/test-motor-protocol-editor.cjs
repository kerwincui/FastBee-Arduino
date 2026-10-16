'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require('../test/browser/node_modules/playwright');
const root = path.resolve(process.env.FASTBEE_TEST_MOTOR_SOURCE || path.join(__dirname, '..'));
const outArg = process.argv.find(value => value.startsWith('--out='));
const positional = process.argv.slice(2).find(value => !value.startsWith('--'));
if (!outArg && !positional) throw new Error('Pass a new output directory; existing evidence is never overwritten');
const out = path.resolve(root, outArg ? outArg.slice(6) : positional);
async function main() {
    fs.mkdirSync(out, { recursive: true });
    const css = fs.readFileSync(path.join(root, 'web-src/css/main.css'), 'utf8');
    const html = fs.readFileSync(path.join(root, 'web-src/pages/fragments/protocol-modbus-rtu.html'), 'utf8');
    const browser = await chromium.launch({ headless: true });
    const results = [];
    try {
        for (const width of [1440, 390]) {
            const page = await browser.newPage({ viewport: { width, height: 900 } });
            const requests = [], errors = [];
            page.on('request', request => requests.push(request.url()));
            page.on('pageerror', error => errors.push(error.message));
            await page.route('**/*', route => route.abort());
            await page.setContent('<style>' + css + '</style>' + html);
            await page.addScriptTag({ path: path.join(root, 'web-src/js/state.js') });
            await page.addScriptTag({ path: path.join(root, 'web-src/modules/runtime/protocol/modbus-config.js') });
            await page.evaluate(() => {
                window.warnings = [];
                window.Notification = { warning: text => window.warnings.push(text) };
                Object.assign(AppState, {
                    guardDeveloperModeAction: () => true,
                    ensureModal: id => document.getElementById(id),
                    showModal: modal => { modal.style.display = 'flex'; modal.style.opacity = '1'; },
                    hideModal: modal => { modal.style.display = 'none'; },
                    toggleVisible: (node, visible) => { node.style.display = visible ? '' : 'none'; },
                    _renderAllDevices: () => {},
                    saveProtocolConfig: async () => true,
                    _modbusDevices: [{ name:'Motor', deviceType:'motor', slaveAddress:8, motorRegs:[0,1,2,5,7],
                        motorMinPosition:-100, motorMaxPosition:100, motorCurrentPosition:0, motorMoveStep:0 }]
                });
                AppState.setupGlobalEventDelegation();
                AppState._openEditModal(0);
            });
            assert.equal(await page.locator('#mdev-edit-motor-profile').count(), 0);
            assert.equal(await page.locator('#mdev-edit-motor-read-speed').inputValue(), '5');
            assert.equal(await page.locator('#mdev-edit-motor-finite').count(), 0);
            await page.locator('#mdev-edit-motor-advanced > summary').click();
            for (const field of ['fwd', 'rev', 'stop']) await page.locator('#mdev-edit-motor-' + field).fill('100');
            await page.locator('#mdev-edit-motor-speed').fill('101');
            await page.locator('#mdev-edit-motor-pulse').fill('102');
            await page.locator('#mdev-edit-motor-read-fc').selectOption('4');
            await page.locator('#mdev-edit-motor-write-fc').selectOption('16');
            await page.locator('#mdev-edit-motor-read-speed').fill('200');
            await page.locator('#mdev-edit-motor-read-pulse').fill('201');
            await page.locator('#mdev-edit-motor-fwd-value').fill('17');
            await page.locator('#mdev-edit-motor-rev-value').fill('34');
            await page.locator('#mdev-edit-motor-stop-value').fill('0');
            // Check field geometry at the actual narrow-screen breakpoint.
            const overflow = await page.locator('#mdev-edit-motor-section').evaluate(section => {
                const width = document.documentElement.clientWidth;
                return [...section.querySelectorAll('input,select')].filter(el => {
                    const box = el.getBoundingClientRect(); return box.width > 0 && (box.left < -1 || box.right > width + 1);
                }).map(el => el.id);
            });
            assert.deepEqual(overflow, []);
            await page.locator('#mdev-edit-motor-protocol-fields').scrollIntoViewIfNeeded();
            await page.screenshot({ path: path.join(out, 'editor-' + width + '.png'), fullPage: true });
            await page.locator('[data-action="_saveEditModalToDevice"]').click();
            const saved = await page.evaluate(() => AppState._modbusDevices[0]);
            assert.deepEqual(saved.motorRegs, [100,100,100,101,102]);
            assert.equal(saved.motorProtocol.stopValue, 0);
            assert.equal(saved.motorProtocol.readFunction, 4);
            assert.equal(saved.motorProtocol.writeFunction, 16);
            assert.equal(saved.motorProtocol.finitePulseMove, true);
            assert.equal(saved.slaveAddress, 8);
            assert.equal(saved.motorMinPosition, -100);
            assert.equal(saved.motorMaxPosition, 100);
            await page.evaluate(() => AppState._openEditModal(0));
            assert.equal(await page.locator('#mdev-edit-motor-stop-value').inputValue(), '0');
            await page.locator('#mdev-edit-motor-advanced > summary').click();
            await page.locator('#mdev-edit-motor-rev-value').fill('17');
            await page.locator('[data-action="_saveEditModalToDevice"]').click();
            assert.equal(await page.evaluate(() => window.warnings.length), 1);
            assert.deepEqual(await page.evaluate(() => AppState._modbusDevices[0]), saved);
            assert.deepEqual(errors, []);
            assert.deepEqual(requests, []);
            results.push({ width, status:'PASS', savedProfile:saved.motorProtocol, deviceRequests:0 });
            await page.close();
        }
        fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify({ status: 'PASS',
            cases: results.map(result => ({ name: 'editor-' + result.width + 'px', status: result.status })),
            results, physicalMotorTested: false }, null, 2));
        console.log('Motor editor: no protocol selector, advanced values, validation and zero requests passed at 1440/390px.');
    } finally { await browser.close(); }
}
main().catch(error => { console.error(error); process.exitCode = 1; });
