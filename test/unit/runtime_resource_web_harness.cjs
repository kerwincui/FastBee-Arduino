const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
const app = { currentPage: 'peripheral' };
const requests = [];
let snapshots = 0;
function element(tag) {
    return { tag, textContent: '', style: {}, children: [], events: {}, removed: false,
        append(...nodes) { this.children.push(...nodes); },
        appendChild(node) { this.children.push(node); },
        setAttribute() {}, addEventListener(type, handler) { this.events[type] = handler; },
        remove() { this.removed = true; }, showModal() { snapshots++; },
        querySelector(tagName) { return this.children.find(child => child.tag === tagName); }
    };
}
const document = { createElement: element, body: element('body') };
const context = vm.createContext({
    AppState: { registerModule(name, methods) { Object.assign(app, methods); } },
    document,
    apiGetFresh(url) {
        assert.equal(url, '/api/peripherals/resources');
        return new Promise((resolve, reject) => requests.push({ resolve, reject }));
    },
    apiGet() { throw new Error('Cached GET forbidden'); },
    apiPost() { throw new Error('Writes forbidden'); },
    setTimeout() { throw new Error('No polling'); },
    setInterval() { throw new Error('No polling'); }
});
vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime/peripheral-io.js'), 'utf8'), context);
const valid = () => ({ success: true, data: {
    profileId: 'comparison-board', claimScope: 'configuration', configurationStatus: 'observed',
    resources: [{ label: 'GPIO', used: 5, configured: 8, capacity: 40 }],
    runtime: { schemaVersion: 1, scope: 'managed_leases', complete: false, hardwareProbed: false,
        activeProfileId: 'active-board',
        i2c: { status: 'observed', leaseCount: 1, sda: 4, scl: 5,
            leases: [{ ownerKnown: false, addressReserved: true, address: 32, cleanupPending: true }] },
        spi: { status: 'busy', leaseCount: null, leases: null },
        uart: { status: 'observed', leaseCount: 0, leases: [], cleanupPending: null },
        controlOutput: { status: 'observed', leaseCount: 1,
            leases: [{ ownerKnown: true, ownerId: '<consumer>', ownerRole: 'pca9685_oe', resourceId: '<img src=x onerror=bad>', pin: 7, cleanupPending: true, cleanupFailed: true }] },
        encoder: { status: 'observed', leaseCount: 1,
            leases: [{ ownerKnown: true, ownerId: 'encoder-1', ownerRole: 'encoder', pinA: 4, pinB: 5, active: false, interruptAttached: false, interruptCleanupPending: true }] },
        dhtRmt: { status: 'observed', leaseCount: 1,
            leases: [{ ownerKnown: false, ownerRole: 'dht_capture', pin: 15, cleanupPending: true, cleanupFailed: true }] },
        ledc: { status: 'observed', leaseCount: 1,
            leases: [{ ownerKnown: true, ownerId: 'servo-1', pin: 18, channel: 2, cleanupFailed: true }] },
        backlightLedc: { status: 'observed', leaseCount: 1, sampleAgeMs: 100,
            leases: [{ ownerKnown: false, ownerRole: 'display_backlight', pin: 45, channel: 3 }] },
        adc: { status: 'observed', leaseCount: 1, unitCount: 1, channelCount: 1, calibrationCount: 2, calibrationCleanupPending: 1,
            units: [{ unit: 0, retained: true, cleanupFailed: true }],
            channels: [{ unit: 0, channel: 1, pin: 4, configurationKnown: true }],
            leases: [{ ownerKnown: true, ownerId: 'adc-input', pin: 4, unit: 0, channel: 1, resolutionBits: 9, cleanupPending: true }] },
        gpio: { status: 'observed', leaseCount: 1,
            leases: [{ ownerKnown: false, ownerRole: 'command_script', pin: 5, mode: 'output', interruptAttached: false, fault: true }] },
        dac: { status: 'unsupported', leaseCount: null, leases: null },
        neopixel: { status: 'observed', leaseCount: 1,
            leases: [{ ownerKnown: true, ownerId: 'rgb', pin: 18, configuredLedCount: 32, writeAttempted: false, releaseAttempted: true, lastReleaseResult: 7, cleanupPending: true }] },
        stepper: { status: 'observed', leaseCount: 1, truncated: true,
            leases: [{ ownerKnown: true, ownerId: 'step', pins: [4, 5, 6, 7], running: false, retainedMask: 5, gpioCleanupPendingMask: 1, cleanupPending: true }] }
    }
} });
const flush = async () => { await Promise.resolve(); await Promise.resolve(); };
(async () => {
    app.openPeripheralResources();
    assert.equal(snapshots, 1); assert.equal(requests.length, 1);
    const firstDialog = app._periphResourceDialog;
    const firstText = firstDialog.querySelector('pre');
    requests.shift().resolve(valid()); await flush();
    assert.match(firstText.textContent, /comparison-board.*active-board/);
    assert.match(firstText.textContent, /GPIO：5 \/ 8 \/ 40/);
    assert.match(firstText.textContent, /I2C：1 项保留/);
    assert.match(firstText.textContent, /SPI：忙或未知/);
    assert.match(firstText.textContent, /待退役/);
    assert.match(firstText.textContent, /退役清理失败/);
    assert.match(firstText.textContent, /退役阶段未知/);
    assert.match(firstText.textContent, /<img src=x onerror=bad>/); // textContent, no HTML insertion.
    assert.match(firstText.textContent, /<consumer> \(pca9685_oe\)/);
    assert.match(firstText.textContent, /编码器ISR槽：1 项保留/);
    assert.match(firstText.textContent, /A\/B 4\/5/);
    assert.match(firstText.textContent, /计数接收 停止；中断 清理待确认/);
    assert.match(firstText.textContent, /NeoPixel对象：1 项保留/);
    assert.match(firstText.textContent, /配置灯珠 32；发送结果 未尝试；释放结果 7/);
    assert.match(firstText.textContent, /步进四相输出：1 项保留/);
    assert.match(firstText.textContent, /列表已截断，不完整/);
    assert.match(firstText.textContent, /IN1–4 4\/5\/6\/7/);
    assert.match(firstText.textContent, /相位输出 停止；保留掩码 5；GPIO待清理掩码 1/);
    assert.match(firstText.textContent, /DHT RMT：1 项保留/);
    assert.match(firstText.textContent, /外设\/脚本LEDC：1 项保留/);
    assert.match(firstText.textContent, /GPIO18\/CH2/);
    assert.match(firstText.textContent, /屏幕背光LEDC：1 项保留/);
    assert.match(firstText.textContent, /屏幕任务发布于 100ms 前/);
    assert.match(firstText.textContent, /受管ADC：1 项保留/);
    assert.match(firstText.textContent, /单元0：释放失败，仍保留/);
    assert.match(firstText.textContent, /GPIO4\/ADC0\/CH1/);
    assert.match(firstText.textContent, /通道记录 ADC0\/CH1 · GPIO4 · 配置已确认/);
    assert.match(firstText.textContent, /返回位数 9/);
    assert.match(firstText.textContent, /受管GPIO：1 项保留/);
    assert.match(firstText.textContent, /模式 output；中断登记 未附加/);
    assert.match(firstText.textContent, /原生DAC：当前构建未支持/);
    assert.match(firstText.textContent, /SDMMC不支持/);
    assert.equal(firstText.innerHTML, undefined);
    assert.match(firstText.textContent, /LEDC.*未知/);
    assert.equal(requests.length, 0);

    const older = app.refreshPeripheralResources(); const oldRequest = requests.shift();
    const newer = app.refreshPeripheralResources(); const newRequest = requests.shift();
    const busy = valid(); busy.data.configurationStatus = 'busy'; busy.data.resources = null;
    newRequest.resolve(busy); await newer;
    assert.match(firstText.textContent, /配置读取忙或未知/);
    oldRequest.reject(new Error('late failure')); await older;
    assert.match(firstText.textContent, /配置读取忙或未知/);

    const closing = app.refreshPeripheralResources(); const closingRequest = requests.shift();
    app.closePeripheralResources();
    assert.equal(firstDialog.removed, true);
    app.openPeripheralResources(); const current = app._periphResourceDialog.querySelector('pre');
    closingRequest.resolve(valid()); await closing;
    assert.match(current.textContent, /读取中/);
    requests.shift().resolve({ success: true, data: {} }); await flush();
    assert.match(current.textContent, /快照不可用/);
    const pageRead = app.refreshPeripheralResources(); const pageRequest = requests.shift();
    app.currentPage = 'other'; pageRequest.resolve(valid()); await pageRead;
    assert.match(current.textContent, /读取中/);
    app.closePeripheralResources();
    const invalid = valid(); invalid.data.runtime.complete = true;
    assert.throws(() => app._peripheralResourcesText(invalid.data));
    const incomplete = valid(); incomplete.data.runtime.i2c.leaseCount = -1;
    assert.match(app._peripheralResourcesText(incomplete.data), /I2C：忙或未知/);
    const contradictory = valid(); contradictory.data.runtime.i2c.leaseCount = 0;
    assert.match(app._peripheralResourcesText(contradictory.data), /I2C：忙或未知/);
    const sensorBusy = valid(); sensorBusy.data.runtime.dhtRmt = { status: 'busy', leases: null, leaseCount: null };
    assert.match(app._peripheralResourcesText(sensorBusy.data), /DHT RMT：忙或未知/);
    const sensorAbsent = valid(); sensorAbsent.data.runtime.dhtRmt = { status: 'unsupported', leases: null, leaseCount: null };
    assert.match(app._peripheralResourcesText(sensorAbsent.data), /DHT RMT：当前构建未支持/);
    for (const [key, label] of [['adc', '受管ADC'], ['gpio', '受管GPIO'], ['dac', '原生DAC'], ['neopixel', 'NeoPixel对象'], ['stepper', '步进四相输出']]) {
        const allocationFailed = valid();
        allocationFailed.data.runtime[key] = { status: 'unknown', reason: 'SNAPSHOT_ALLOCATION_FAILED', leaseCount: null, leases: null };
        assert.ok(app._peripheralResourcesText(allocationFailed.data).includes(label + '：忙或未知'));
    }
    const adcEmpty = valid(); adcEmpty.data.runtime.adc.leases = []; adcEmpty.data.runtime.adc.leaseCount = 0;
    assert.match(app._peripheralResourcesText(adcEmpty.data), /受管ADC：0 项保留/);
    assert.match(app._peripheralResourcesText(adcEmpty.data), /单元0：释放失败，仍保留/);
    adcEmpty.data.runtime.adc.unitCount = 0;
    assert.match(app._peripheralResourcesText(adcEmpty.data), /ADC单元记录：未知/);
    const gpioOverflow = valid(); gpioOverflow.data.runtime.gpio.leases = Array(50).fill({ pin: 0 });
    gpioOverflow.data.runtime.gpio.leaseCount = 50;
    assert.match(app._peripheralResourcesText(gpioOverflow.data), /受管GPIO：忙或未知/);
    const irqUnknown = valid(); irqUnknown.data.runtime.gpio.leases[0].interruptCleanupPending = true;
    assert.match(app._peripheralResourcesText(irqUnknown.data), /中断登记 清理待确认/);
    const noLights = valid(); noLights.data.runtime.neopixel = { status: 'observed', leaseCount: 0, leases: [] };
    assert.match(app._peripheralResourcesText(noLights.data), /NeoPixel对象：0 项保留/);
    const overSteps = valid(); overSteps.data.runtime.stepper.leases = Array(17).fill({});
    overSteps.data.runtime.stepper.leaseCount = 17;
    assert.match(app._peripheralResourcesText(overSteps.data), /步进四相输出：忙或未知/);
    assert.equal(requests.length, 0);

    const landing = { currentPage: 'peripheral' };
    let hidden, loaded, opens = 0;
    landing.onPageHide = (key, callback) => { hidden = callback; };
    const lazyContext = vm.createContext({
        window: {}, document: { getElementById() { return null; } },
        AppState: { registerModule(name, methods) { Object.assign(landing, methods); } },
        ModuleLoader: { loadModule(name, callback) { assert.equal(name, 'peripheral-io'); loaded = callback; } }
    });
    vm.runInContext(fs.readFileSync(path.join(root, 'web-src/modules/runtime/peripherals.js'), 'utf8'), lazyContext);
    landing.setupPeripheralsEvents(); landing._openPeripheralResources();
    hidden(); // Navigate away and back before the lazy module arrives.
    landing.openPeripheralResources = () => { opens++; };
    loaded(); assert.equal(opens, 0);
    landing._openPeripheralResources(); assert.equal(opens, 1);
})().catch(error => { console.error(error); process.exitCode = 1; });
