'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { chromium } = require('../test/browser/node_modules/playwright');
const root = path.resolve(__dirname, '..');
const out = process.argv[2];
if (!out) throw new Error('Pass a new output directory; existing evidence is never overwritten');
fs.mkdirSync(out, { recursive: false });
const files = ['web-src/pages/fragments/protocol-modbus-rtu.html', 'web-src/css/main.css',
    'web-src/modules/runtime/protocol/modbus-relay-motor.js', 'web-src/js/utils.js'];
const sources = files.map(file => fs.readFileSync(path.join(root, file), 'utf8'));
const sourceFiles = Object.fromEntries(files.map((file, i) => [file,
    crypto.createHash('sha256').update(sources[i]).digest('hex')]));
const results = [], network = [], errors = [];

(async () => {
    const browser = await chromium.launch({ headless: true });
    try {
        for (const width of [1280, 390, 320]) {
            const page = await browser.newPage({ viewport: { width, height: 900 } });
            page.on('pageerror', error => errors.push(error.message));
            await page.route('**/*', route => { network.push(route.request().url()); return route.abort(); });
            await page.setContent('<!doctype html><meta charset="utf-8"><style>' + sources[1] + '</style>' + sources[0]);
            await page.evaluate(() => {
                const modal = document.getElementById('modbus-device-ctrl-modal');
                document.body.replaceChildren(modal);
                modal.style.display = 'flex';
                document.getElementById('modbus-motor-panel').classList.remove('fb-hidden');
                document.getElementById('modbus-relay-panel').classList.add('fb-hidden');
                document.querySelector('.motor-travel-section').open = true;
                window.AppState = { _activeDeviceIdx: 0, _modbusCtrlGeneration: 1,
                    _modbusDevices: [{ slaveAddress: 19 }] };
                window.posts = [];
                window.notifications = [];
                window.Notification = { error: text => notifications.push(text), success: text => notifications.push(text) };
                window.apiPostPriority = async (url, params) => { posts.push({ url, params }); return { success: true }; };
            });
            await page.addScriptTag({ content: sources[3] });
            await page.addScriptTag({ content: sources[2] });
            await page.evaluate(() => {
                document.addEventListener('click', event => {
                    const button = event.target.closest('[data-action]');
                    if (button && ['confirmMotorReference', 'confirmMotorStationary'].includes(button.dataset.action))
                        AppState[button.dataset.action]();
                });
                window.fresh = changes => AppState._applyProtocolMotorState(Object.assign({
                    motorRef: 'motor.19.test', travelToken: '12345678-00000001', softLimitEnabled: true,
                    referenceKnown: false, stationaryConfirmed: false, parametersKnown: true, speed: 10, pulse: 20
                }, changes));
                fresh({ softLimitEnabled: false });
            });
            const reference = page.locator('[data-action="confirmMotorReference"]');
            const stationary = page.locator('#motor-travel-stationary');
            await stationary.check();
            await page.locator('#motor-travel-reference').fill('0');
            await reference.click();
            assert.equal(await page.evaluate(() => posts.length), 0, 'unconfigured limits block confirmation');
            await page.evaluate(() => fresh({ travelToken: '12345678-00000002' }));
            assert.equal(await stationary.isChecked(), false);
            await reference.click();
            assert.equal(await page.evaluate(() => posts.length), 0, 'onsite assertion is required');
            await stationary.check();
            await page.locator('#motor-travel-reference').fill('1.5');
            await reference.click();
            assert.equal(await page.evaluate(() => posts.length), 0, 'fractional controller counts are rejected');
            await page.locator('#motor-travel-reference').fill('-79');
            await reference.click();
            assert.equal(await page.evaluate(() => posts.length), 1);
            assert.equal(await stationary.isChecked(), false);
            await stationary.check();
            await reference.click();
            assert.equal(await page.evaluate(() => posts.length), 1, 'a consumed state cannot be replayed');
            await page.evaluate(() => fresh({ travelToken: '12345678-00000003', referenceKnown: true }));
            await stationary.check();
            await page.locator('[data-action="confirmMotorStationary"]').click();
            const posts = await page.evaluate(() => window.posts);
            assert.deepEqual(posts.map(post => post.params.action), ['confirmReference', 'confirmStationary']);
            assert(posts.every(post => post.url === '/api/modbus/motor/control'));
            assert.equal(posts[0].params.position, '-79');
            assert.equal(posts[1].params.position, undefined);
            await page.evaluate(() => fresh({ travelToken: '12345678-00000004', referenceKnown: true,
                stationaryConfirmed: true, possiblePositionMin: -79, possiblePositionMax: -59 }));
            const layout = await page.locator('.motor-travel-section').evaluate(section => {
                const rect = section.getBoundingClientRect();
                const elements = Array.from(section.querySelectorAll('label,input,button,span'));
                return { width: rect.width, overflow: section.scrollWidth > section.clientWidth + 1,
                    viewportOverflow: document.documentElement.scrollWidth > innerWidth + 1,
                    clipped: elements.filter(element => {
                        const box = element.getBoundingClientRect();
                        return box.left < rect.left - 1 || box.right > rect.right + 1 || element.scrollWidth > element.clientWidth + 1;
                    }).map(element => element.id || element.className || element.tagName),
                    labelWhitespace: getComputedStyle(section.querySelector('label')).whiteSpace };
            });
            assert(!layout.overflow && !layout.viewportOverflow, JSON.stringify({ width, layout }));
            assert.deepEqual(layout.clipped, [], JSON.stringify({ width, layout }));
            assert.notEqual(layout.labelWhitespace, 'nowrap');
            await page.locator('.motor-travel-section').scrollIntoViewIfNeeded();
            await page.screenshot({ path: path.join(out, 'travel-' + width + '.png'), fullPage: true });
            results.push({ width, status: 'PASS', layout, confirmationPosts: posts.length, motionCommands: 0 });
            await page.close();
        }
        assert.deepEqual(network, [], 'offline test must not contact devices or the network');
        assert.deepEqual(errors, []);
    } finally {
        await browser.close();
    }
})().then(() => {
    const sourceStable = files.every((file, i) => fs.readFileSync(path.join(root, file), 'utf8') === sources[i]);
    assert(sourceStable);
    const report = { status: 'PASS', at: new Date().toISOString(), sourceFiles, sourceStable,
        results, networkRequests: network.length, errors, deviceRequests: 0, physicalAcceptance: 'NOT_RUN' };
    fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(report, null, 2), { flag: 'wx' });
    console.log(JSON.stringify(report));
}).catch(error => {
    fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify({ status: 'FAIL', results, network, errors,
        error: error.stack, sourceFiles, at: new Date().toISOString() }, null, 2), { flag: 'wx' });
    console.error(error);
    process.exitCode = 1;
});
