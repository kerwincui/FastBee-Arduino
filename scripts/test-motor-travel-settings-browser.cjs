'use strict';
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const { chromium } = require('../test/browser/node_modules/playwright');
const root = path.resolve(process.env.FASTBEE_TEST_MOTOR_SOURCE || path.join(__dirname, '..'));
const outArg = process.argv.find(value => value.startsWith('--out='));
if (!outArg) throw Error('Provide --out=<unused-directory>');
const out = path.resolve(root, outArg.slice(6));
if (fs.existsSync(out)) throw Error('Preserve previous browser evidence');
fs.mkdirSync(out, { recursive: true });
const report = { status: 'RUNNING', results: [], deviceRequests: 0, motorActions: 0 };
const persist = () => fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(report, null, 2));

async function main() {
    const browser = await chromium.launch({ headless: true });
    try {
        for (const width of [1280, 390, 320]) {
            const page = await browser.newPage({ viewport: { width, height: 900 } });
            const requests = [], errors = [];
            page.on('request', request => requests.push(request.url()));
            page.on('pageerror', error => errors.push(error.message));
            await page.route('**/*', route => route.abort());
            await page.setContent('<style>' + fs.readFileSync(path.join(root, 'web-src/css/main.css'), 'utf8') + '</style>' +
                fs.readFileSync(path.join(root, 'web-src/pages/fragments/protocol-modbus-rtu.html'), 'utf8'));
            await page.addScriptTag({ path: path.join(root, 'web-src/js/state.js') });
            await page.addScriptTag({ path: path.join(root, 'web-src/modules/runtime/protocol/modbus-config.js') });
            await page.evaluate(() => {
                window.warnings = [];
                window.Notification = { warning: text => window.warnings.push(text) };
                Object.assign(AppState, { guardDeveloperModeAction: () => true,
                    ensureModal: id => document.getElementById(id),
                    showModal: modal => { modal.style.display = 'flex'; modal.style.opacity = '1'; },
                    hideModal: modal => { modal.style.display = 'none'; },
                    toggleVisible: (element, visible) => { element.style.display = visible ? '' : 'none'; },
                    _renderAllDevices() {}, _closeCtrlModal() { this._activeDeviceIdx = -1; },
                    async saveProtocolConfig() { return true; }, _modbusDevices: []
                });
                AppState.setupGlobalEventDelegation();
                AppState._openEditModal(-1);
            });
            await page.locator('#mdev-edit-type').selectOption('motor');
            await page.locator('#mdev-edit-name').fill('电机');
            await page.locator('#mdev-edit-addr').fill('8');
            assert.equal(await page.locator('#mdev-edit-motor-profile').count(), 0);
            for (const id of ['mdev-edit-ch', 'mdev-edit-base', 'mdev-edit-protocol', 'mdev-edit-nc', 'mdev-edit-motor-fwd', 'mdev-edit-motor-min-pos'])
                assert.equal(await page.locator('#' + id).isVisible(), false, id);
            for (const id of ['mdev-edit-motor-cur-pos', 'mdev-edit-motor-move-step', 'mdev-edit-motor-last-pulse', 'mdev-edit-motor-finite', 'mdev-edit-travel-countsPerMm'])
                assert.equal(await page.locator('#' + id).count(), 0, id);
            await page.screenshot({ path: path.join(out, 'simple-' + width + '.png'), fullPage: true });
            await page.locator('[data-action="_saveEditModalToDevice"]').click();
            const saved = await page.evaluate(() => structuredClone(AppState._modbusDevices[0]));
            assert.equal(saved.sensorId, 'motor_8'); assert.equal(saved.slaveAddress, 8);
            assert.deepEqual(saved.motorRegs, [0, 1, 2, 5, 7]);
            assert.equal(saved.motorMinPosition, 0); assert.equal(saved.motorMaxPosition, 0);
            assert.equal(saved.motorProtocol.finitePulseMove, true);
            await page.evaluate(() => { AppState._activeDeviceIdx = 0; AppState.openMotorTravelSettings(); });
            await page.locator('#mdev-edit-motor-limits').check();
            await page.locator('#mdev-edit-motor-min-pos').fill('-200');
            await page.locator('#mdev-edit-motor-max-pos').fill('300');
            const overflow = await page.locator('#mdev-edit-motor-section').evaluate(section => {
                const viewport = document.documentElement.clientWidth;
                return [...section.querySelectorAll('input,select,summary')].filter(element => {
                    const box = element.getBoundingClientRect(); return box.width > 0 && (box.left < -1 || box.right > viewport + 1);
                }).map(element => element.id || element.tagName);
            });
            assert.deepEqual(overflow, []);
            await page.screenshot({ path: path.join(out, 'limits-' + width + '.png'), fullPage: true });
            await page.locator('[data-action="_saveEditModalToDevice"]').click();
            const limited = await page.evaluate(() => structuredClone(AppState._modbusDevices[0]));
            assert.equal(limited.motorMinPosition, -200); assert.equal(limited.motorMaxPosition, 300);
            assert.equal(limited.motorProtocol.positionTracking, 'commanded'); assert.equal(limited.motorProtocol.startupPosition, 0);
            await page.evaluate(() => { AppState._activeDeviceIdx = 0; AppState.openMotorTravelSettings(); });
            assert.equal(await page.locator('#mdev-edit-motor-limits').isChecked(), true);
            await page.locator('#mdev-edit-motor-limits').uncheck();
            await page.locator('[data-action="_saveEditModalToDevice"]').click();
            const manual = await page.evaluate(() => AppState._modbusDevices[0]);
            assert.equal(manual.motorMinPosition, 0); assert.equal(manual.motorMaxPosition, 0);
            assert.equal(manual.motorProtocol.startupPosition, undefined);
            assert.deepEqual(await page.evaluate(() => window.warnings), []);
            assert.deepEqual(requests, []); assert.deepEqual(errors, []);
            report.results.push({ width, status:'PASS', defaultSave:true, optionalLimits:true, removedFields:true, noOverflow:true });
            persist(); await page.close();
        }
        report.status = 'PASS';
    } catch (error) { report.status = 'FAIL'; report.error = error.message; throw error; }
    finally { persist(); await browser.close(); }
    console.log(JSON.stringify(report));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
