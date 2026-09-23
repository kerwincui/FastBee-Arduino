'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const {
    CAPABILITY_WEB_ASSET_BUDGETS,
    DEFAULT_WEB_ASSET_BUDGETS,
    collectPublishedHtmlDomStats,
    countStaticHtmlNodes,
    evaluateStaticHtmlDomStats,
    evaluateWebAssetReport,
    resolveWebAssetBudgets,
    runWebQualityGate,
    validateLifecycleContracts,
    validateResourceManifest
} = require('./web-quality-gate');
const { PAGE_BUNDLES } = require('./web-asset-report');

[
    'js/modules/periph-exec.js',
    'js/modules/periph-exec-form.js',
    'js/modules/periph-exec-sensor.js',
    'js/modules/periph-exec-effects.js',
    'js/modules/periph-exec-modbus.js',
    'js/modules/peripheral-save.js',
    'js/modules/peripheral-io.js'
].forEach((asset) => {
    assert.ok(PAGE_BUNDLES.includes(asset), `${asset} must be covered by the runtime bundle budget`);
});

function report(overrides = {}) {
    return {
        totals: { gzip: 1000, missingGzip: 0, files: 2, ...(overrides.totals || {}) },
        boot: { gzip: 500, files: ['index.html'], ...(overrides.boot || {}) },
        assets: overrides.assets || [{ path: 'index.html', raw: 1000, gzip: 500, hasGzip: true }],
        largest: overrides.assets || [{ path: 'index.html', raw: 1000, gzip: 500, hasGzip: true }],
        pageBundles: overrides.pageBundles || []
    };
}

assert.deepStrictEqual(evaluateWebAssetReport(report()), []);
assert.ok(evaluateWebAssetReport(report({
    totals: { gzip: DEFAULT_WEB_ASSET_BUDGETS.totalGzip + 1 }
})).some((issue) => issue.includes('total gzip')));
assert.ok(evaluateWebAssetReport(report({
    boot: { gzip: DEFAULT_WEB_ASSET_BUDGETS.bootGzip + 1 }
})).some((issue) => issue.includes('boot gzip')));
assert.ok(evaluateWebAssetReport(report({
    assets: [{ path: 'large.css', raw: 1, gzip: DEFAULT_WEB_ASSET_BUDGETS.singleAssetGzip + 1 }]
})).some((issue) => issue.includes('oversized asset')));
assert.ok(evaluateWebAssetReport(report({
    pageBundles: [{ path: 'js/modules/large.js', raw: 1, gzip: DEFAULT_WEB_ASSET_BUDGETS.runtimeBundleGzip + 1 }]
})).some((issue) => issue.includes('oversized runtime bundle')));
assert.ok(evaluateWebAssetReport(report({
    totals: { missingGzip: 1 }
})).some((issue) => issue.includes('missing gzip')));
assert.ok(evaluateWebAssetReport(report({
    assets: Array.from({ length: DEFAULT_WEB_ASSET_BUDGETS.gzipFiles + 1 }, (_, index) => ({
        path: `asset-${index}.js`, raw: 1, gzip: 1, hasGzip: true
    }))
})).some((issue) => issue.includes('gzip file count')));

assert.strictEqual(countStaticHtmlNodes(
    '<!doctype html><html><body><div><span>x</span></div>' +
    '<script>const fake = "<section><p>not DOM</p></section>";</script></body></html>'
), 5, 'script strings must not inflate the static DOM count');
assert.deepStrictEqual(evaluateStaticHtmlDomStats({
    totalNodes: 10,
    files: [{ path: 'index.html', nodes: 10 }]
}), []);
assert.ok(evaluateStaticHtmlDomStats({
    totalNodes: DEFAULT_WEB_ASSET_BUDGETS.staticHtmlNodesTotal + 1,
    files: []
}).some((issue) => issue.includes('static HTML DOM nodes')));
assert.ok(evaluateStaticHtmlDomStats({
    totalNodes: 1,
    files: [{ path: 'large.html', nodes: DEFAULT_WEB_ASSET_BUDGETS.singleHtmlNodes + 1 }]
}).some((issue) => issue.includes('oversized HTML DOM')));

const panelAllocation = CAPABILITY_WEB_ASSET_BUDGETS.screenPanel;
const panelManifest = {
    schemaVersion: 1,
    environment: 'esp32s3-F16R8',
    capabilityFingerprint: 'fixture',
    runtimeConfigurationAffectsResources: false,
    compiledCapabilities: { screenPanel: true },
    resources: [...panelAllocation.assets]
};
const panelBudgets = resolveWebAssetBudgets(panelManifest);
assert.strictEqual(panelBudgets.totalGzip,
    DEFAULT_WEB_ASSET_BUDGETS.totalGzip + panelAllocation.lazyGzip + panelAllocation.sharedGzip);
assert.strictEqual(panelBudgets.gzipFiles, 57);
assert.strictEqual(panelBudgets.staticHtmlNodesTotal, 3320);
['bootGzip', 'singleAssetGzip', 'runtimeBundleGzip', 'singleHtmlNodes'].forEach((key) => {
    assert.strictEqual(panelBudgets[key], DEFAULT_WEB_ASSET_BUDGETS[key], `${key} must not be relaxed`);
});
assert.strictEqual(DEFAULT_WEB_ASSET_BUDGETS.totalGzip, 279 * 1024, 'base limits remain immutable');
[
    null,
    { ...panelManifest, compiledCapabilities: { screenPanel: false } },
    { ...panelManifest, compiledCapabilities: { screenPanel: 'true' } },
    { ...panelManifest, compiledCapabilities: {}, features: { screenPanel: { enabled: true } } },
    { ...panelManifest, schemaVersion: 2 },
    { ...panelManifest, runtimeConfigurationAffectsResources: true },
    { ...panelManifest, capabilityFingerprint: '' },
    { ...panelManifest, resources: panelAllocation.assets.slice(1) }
].forEach((manifest) => {
    const budgets = resolveWebAssetBudgets(manifest);
    assert.strictEqual(budgets.totalGzip, DEFAULT_WEB_ASSET_BUDGETS.totalGzip);
    assert.strictEqual(budgets.gzipFiles, DEFAULT_WEB_ASSET_BUDGETS.gzipFiles);
    assert.strictEqual(budgets.staticHtmlNodesTotal, DEFAULT_WEB_ASSET_BUDGETS.staticHtmlNodesTotal);
    assert.deepStrictEqual(budgets.capabilityAllocations, []);
});

function panelReport({ baseGzip = DEFAULT_WEB_ASSET_BUDGETS.totalGzip,
    baseFiles = DEFAULT_WEB_ASSET_BUDGETS.gzipFiles,
    sizes = [16 * 1024, 4 * 1024, 4 * 1024, 4 * 1024] } = {}) {
    const base = Array.from({ length: baseFiles }, (_, index) => ({
        path: index === 0 ? 'index.html' : `base-${index}.js`,
        gzip: Math.floor(baseGzip / baseFiles) + (index < baseGzip % baseFiles ? 1 : 0),
        hasGzip: true
    }));
    const lazy = panelAllocation.assets.map((asset, index) => ({
        path: asset, gzip: sizes[index], hasGzip: true
    }));
    const assets = [...base, ...lazy];
    return report({
        assets,
        totals: { gzip: assets.reduce((sum, item) => sum + item.gzip, 0), files: assets.length }
    });
}
const boundaryReport = panelReport({
    baseGzip: DEFAULT_WEB_ASSET_BUDGETS.totalGzip + panelAllocation.sharedGzip
});
assert.deepStrictEqual(evaluateWebAssetReport(boundaryReport, panelBudgets), []);
assert.ok(evaluateWebAssetReport(boundaryReport).some((issue) => issue.includes('total gzip')),
    'the same published size must fail without a compiled capability allocation');
assert.ok(evaluateWebAssetReport(panelReport({
    baseGzip: 1000,
    sizes: [16 * 1024, 4 * 1024, 4 * 1024, panelAllocation.lazyGzip - 24 * 1024 + 1]
}), panelBudgets).some((issue) => issue.includes('screenPanel lazy gzip')),
    'lazy resources cannot spend unused base space');
assert.ok(evaluateWebAssetReport(panelReport({
    baseGzip: DEFAULT_WEB_ASSET_BUDGETS.totalGzip + panelAllocation.sharedGzip + 1,
    sizes: [1000, 1000, 1000, 1000]
}), panelBudgets).some((issue) => issue.includes('base/shared gzip')),
    'base growth cannot spend unused lazy-page space');
assert.ok(evaluateWebAssetReport(panelReport({ baseFiles: 54 }), panelBudgets)
    .some((issue) => issue.includes('base gzip file count')));
const missingPanel = panelReport();
missingPanel.assets = missingPanel.assets.filter((item) => item.path !== 'css/screen-panel.css');
assert.ok(evaluateWebAssetReport(missingPanel, panelBudgets)
    .some((issue) => issue.includes('lazy asset set is incomplete')));
const eagerPanel = panelReport();
eagerPanel.boot.files.push('js/modules/screen-panel.js');
assert.ok(evaluateWebAssetReport(eagerPanel, panelBudgets)
    .some((issue) => issue.includes('outside the boot resource set')));
const oversizedBoot = panelReport();
oversizedBoot.boot.gzip = DEFAULT_WEB_ASSET_BUDGETS.bootGzip + 1;
assert.ok(evaluateWebAssetReport(oversizedBoot, panelBudgets)
    .some((issue) => issue.includes('boot gzip')));
const oversizedRuntime = panelReport();
oversizedRuntime.pageBundles = [{ path: 'js/modules/peripherals.js',
    gzip: DEFAULT_WEB_ASSET_BUDGETS.runtimeBundleGzip + 1 }];
assert.ok(evaluateWebAssetReport(oversizedRuntime, panelBudgets)
    .some((issue) => issue.includes('oversized runtime bundle')));
panelAllocation.assets.filter((asset) => asset.endsWith('.js')).forEach((asset) => {
    const oversizedPanel = panelReport();
    oversizedPanel.pageBundles = [{ path: asset,
        gzip: DEFAULT_WEB_ASSET_BUDGETS.runtimeBundleGzip + 1 }];
    assert.ok(evaluateWebAssetReport(oversizedPanel, panelBudgets)
        .some((issue) => issue.includes('oversized runtime bundle')),
    `${asset} must keep the same runtime module limit`);
});
const oversizedAsset = panelReport();
oversizedAsset.assets[0].gzip = DEFAULT_WEB_ASSET_BUDGETS.singleAssetGzip + 1;
assert.ok(evaluateWebAssetReport(oversizedAsset, panelBudgets)
    .some((issue) => issue.includes('oversized asset')));

function panelDom(baseNodes, panelNodes) {
    return { totalNodes: baseNodes + panelNodes, files: [
        // Split base nodes across pages so the independent single-page limit holds.
        ...Array.from({ length: 4 }, (_, index) => ({ path: `page-${index}.html`,
            nodes: Math.floor(baseNodes / 4) + (index < baseNodes % 4 ? 1 : 0) })),
        { path: 'pages/screen-panel.html', nodes: panelNodes }
    ] };
}
assert.deepStrictEqual(evaluateStaticHtmlDomStats(panelDom(3100, 220), panelBudgets), []);
assert.ok(evaluateStaticHtmlDomStats(panelDom(3100, 220))
    .some((issue) => issue.includes('static HTML DOM nodes')));
assert.ok(evaluateStaticHtmlDomStats(panelDom(100, 221), panelBudgets)
    .some((issue) => issue.includes('screenPanel static HTML DOM nodes')));
assert.ok(evaluateStaticHtmlDomStats(panelDom(3101, 100), panelBudgets)
    .some((issue) => issue.includes('base static HTML DOM nodes')));
const oversizedPage = panelDom(100, 200);
oversizedPage.files[0].nodes = DEFAULT_WEB_ASSET_BUDGETS.singleHtmlNodes + 1;
assert.ok(evaluateStaticHtmlDomStats(oversizedPage, panelBudgets)
    .some((issue) => issue.includes('oversized HTML DOM')));

assert.deepStrictEqual(validateLifecycleContracts(), [],
    'the checked-in page lifecycle must provide replacement and cleanup paths');

const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-web-quality-'));
try {
    fs.writeFileSync(path.join(tempDir, 'index.html.gz'), 'fixture', 'utf8');
    const capabilities = Object.fromEntries([
        'deviceControl', 'modbus', 'ota', 'ruleScript', 'logViewer', 'userAdmin', 'irRemote', 'rfLearn'
    ].map((name) => [name, false]));
    fs.writeFileSync(path.join(tempDir, 'web-resource-manifest.json'), JSON.stringify({
        schemaVersion: 1,
        environment: 'test',
        capabilityFingerprint: 'fixture',
        runtimeConfigurationAffectsResources: false,
        compiledCapabilities: capabilities,
        resources: ['index.html']
    }), 'utf8');
    assert.deepStrictEqual(validateResourceManifest(tempDir), []);
    fs.writeFileSync(path.join(tempDir, 'undeclared.png'), 'fixture', 'utf8');
    assert.ok(validateResourceManifest(tempDir).some((issue) => issue.includes('missing from manifest')));
} finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
}

const domTempDir = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-web-dom-'));
try {
    fs.writeFileSync(path.join(domTempDir, 'page.html'), '<main><p>one</p><p>two</p></main>', 'utf8');
    const stats = collectPublishedHtmlDomStats(domTempDir);
    assert.strictEqual(stats.totalNodes, 3);
    assert.deepStrictEqual(stats.files, [{ path: 'page.html', nodes: 3 }]);
} finally {
    fs.rmSync(domTempDir, { recursive: true, force: true });
}

runWebQualityGate();
console.log('Web quality gate tests OK');
