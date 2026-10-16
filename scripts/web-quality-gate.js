'use strict';

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const {
    ASSET_CAPABILITY_REQUIREMENTS,
    RESOURCE_MANIFEST_NAME,
    readWebResourceManifest,
    shouldIncludeAsset
} = require('./web-profile');
const { createWebAssetReport, formatBytes } = require('./web-asset-report');

const ROOT_DIR = path.resolve(__dirname, '..');
const WWW_DIR = path.join(ROOT_DIR, 'data', 'www');
const WEB_SOURCE_DIRS = [
    path.join(ROOT_DIR, 'web-src', 'js'),
    path.join(ROOT_DIR, 'web-src', 'modules')
];

// esp32s3-F16R8 measured 2026-09-14 with the on-demand 55-item experiment
// catalog: total 285132 B, boot 62038 B, catalog bundle 15537 B.
// 2026-09-30: reserve a bounded 2 KiB for SSE liveness, stale-snapshot and
// serialized relay recovery. The first COM15 repair measured 316.0 KiB total
// and 277.8 KiB base/shared. Keep boot, individual asset, bundle and DOM
// ceilings unchanged; filesystem capacity is checked again before deployment.
// 2026-10-03: the authorized system plan adds transactional migration,
// passive runtime-resource diagnostics, typed templates and quality/status
// handling. The measured r9->r19-r2 increase is about 28 KiB after removing
// unused code. Allocate a bounded 32 KiB common-resource reserve for that
// scope, not a compression claim. Boot/per-asset/DOM limits are unchanged.
// Evidence and acceptance boundaries: docs/system-improvement-web-resource-budget-20261003.md.
const SYSTEM_PLAN_SHARED_RESERVE = 32 * 1024;
const DEFAULT_WEB_ASSET_BUDGETS = Object.freeze({
    totalGzip: 281 * 1024 + SYSTEM_PLAN_SHARED_RESERVE,
    bootGzip: 66 * 1024,
    singleAssetGzip: 26 * 1024,
    runtimeBundleGzip: 16 * 1024,
    gzipFiles: 53,
    staticHtmlNodesTotal: 3100,
    singleHtmlNodes: 920
});

// esp32s3-F16R8 measured 2026-09-17 after rotation, three-page templates,
// forecast and history configuration: screenPanel's four lazy assets are
// 31004 B gzip. Touch optimization reallocates 4.5 KiB of the shared reserve to
// the four lazy assets after retiring remote-resource UI. The industrial UI
// snapshot measures base/shared 285149 B, below the 279 KiB base allowance;
// the remaining 0.5 KiB shared reserve is sufficient. Combined allowance
// stays 36 KiB before the base reservation below; total, boot, per-asset and
// DOM limits remain unchanged. Configuration conflict recovery and slider-size
// validation measure 38788 B lazy, 279373 B base/shared, and 318161 B total.
// Reserve 3 KiB from base/shared headroom: the resulting 276.5 KiB base/shared
// ceiling still exceeds the measured base by 3763 B. The combined ceiling
// was 315 KiB. The separately measured recovery reserve above makes the
// pre-plan combined ceiling 317 KiB and base/shared ceiling 278.5 KiB.
// The system-plan shared reserve above applies in addition to this history.
// 2026-10-16: the screen UI refresh (theme variables, status-dot glow and
// layout edge metrics) measures 40539 B lazy gzip on esp32s3-F16R8
// (screen-panel.js 16380 + screen-panel-model.js 15702 + screen-panel.html
// 3784 + screen-panel.css 4673). The B7 build-time CSS strip already removes
// every provably dead rule and both panel modules are terser-minified with
// property mangling, so no behavior-preserving slimming remains; the extra
// space is borrowed from base/shared headroom (measured 313363 B, about
// 2.0 KiB below the resulting allowance) per the note at the allocation below.
const CAPABILITY_WEB_ASSET_BUDGETS = Object.freeze({
    screenPanel: Object.freeze({
        assets: Object.freeze([
            'js/modules/screen-panel.js',
            'js/modules/screen-panel-model.js',
            'pages/screen-panel.html',
            'css/screen-panel.css'
        ]),
        // Ordering, the sleep switch and the 2026-10-16 screen UI refresh
        // (measured 40539 B lazy, 91 B over the old allowance) borrow 2.5 KiB
        // from base/shared headroom; combined publish, boot and per-bundle
        // ceilings remain unchanged.
        lazyGzip: 41 * 1024,
        sharedGzip: 0.5 * 1024,
        reallocatedBaseGzip: 5.5 * 1024,
        gzipFiles: 4,
        staticHtmlNodesTotal: 220
    })
});

function resolveWebAssetBudgets(manifest, baseBudgets = DEFAULT_WEB_ASSET_BUDGETS) {
    const budgets = { ...baseBudgets, capabilityAllocations: [] };
    // Runtime settings and loose truthy values must never grant publish space.
    // Missing/invalid manifests retain the base limits and fail manifest checks.
    if (!manifest || manifest.schemaVersion !== 1 ||
        manifest.runtimeConfigurationAffectsResources !== false ||
        !manifest.environment || !manifest.capabilityFingerprint ||
        !Array.isArray(manifest.resources)) return budgets;
    Object.entries(CAPABILITY_WEB_ASSET_BUDGETS).forEach(([capability, allocation]) => {
        if (!manifest.compiledCapabilities || manifest.compiledCapabilities[capability] !== true ||
            !allocation.assets.every((asset) => manifest.resources.includes(asset))) return;
        budgets.capabilityAllocations.push({ capability, ...allocation });
        budgets.totalGzip += allocation.lazyGzip + allocation.sharedGzip - (allocation.reallocatedBaseGzip || 0);
        budgets.gzipFiles += allocation.gzipFiles;
        budgets.staticHtmlNodesTotal += allocation.staticHtmlNodesTotal;
    });
    return budgets;
}

function normalizeRel(value) {
    return String(value || '').replace(/\\/g, '/').replace(/^\.\//, '');
}

function walkFiles(directory, files = []) {
    if (!fs.existsSync(directory)) return files;
    fs.readdirSync(directory, { withFileTypes: true }).forEach((entry) => {
        const filePath = path.join(directory, entry.name);
        if (entry.isDirectory()) walkFiles(filePath, files);
        else if (entry.isFile()) files.push(filePath);
    });
    return files;
}

function evaluateWebAssetReport(report, budgets = DEFAULT_WEB_ASSET_BUDGETS) {
    const issues = [];
    if (!report || !report.totals || !report.boot) return ['Web asset report is incomplete'];

    if (report.totals.missingGzip > 0) {
        issues.push(`${report.totals.missingGzip} HTML/JS/CSS asset(s) are missing gzip output`);
    }
    if (report.totals.gzip > budgets.totalGzip) {
        issues.push(`total gzip ${formatBytes(report.totals.gzip)} exceeds ${formatBytes(budgets.totalGzip)}`);
    }
    if (report.boot.gzip > budgets.bootGzip) {
        issues.push(`boot gzip ${formatBytes(report.boot.gzip)} exceeds ${formatBytes(budgets.bootGzip)}`);
    }

    const gzipFiles = (report.assets || []).filter((item) => item.hasGzip).length;
    if (gzipFiles > budgets.gzipFiles) {
        issues.push(`gzip file count ${gzipFiles} exceeds ${budgets.gzipFiles}`);
    }

    const oversizedAssets = (report.assets || report.largest || []).filter((item) =>
        (item.gzip || item.raw || 0) > budgets.singleAssetGzip);
    if (oversizedAssets.length > 0) {
        issues.push(`oversized asset(s): ${oversizedAssets.map((item) =>
            `${item.path}=${formatBytes(item.gzip || item.raw || 0)}`).join(', ')}`);
    }

    const oversizedRuntimeBundles = (report.pageBundles || []).filter((item) =>
        (item.gzip || item.raw || 0) > budgets.runtimeBundleGzip);
    if (oversizedRuntimeBundles.length > 0) {
        issues.push(`oversized runtime bundle(s): ${oversizedRuntimeBundles.map((item) =>
            `${item.path}=${formatBytes(item.gzip || item.raw || 0)}`).join(', ')}`);
    }
    let lazyGzip = 0;
    let lazyGzipLimit = 0;
    let lazyFiles = 0;
    let lazyFileLimit = 0;
    (budgets.capabilityAllocations || []).forEach((allocation) => {
        const assets = (report.assets || []).filter((item) => allocation.assets.includes(item.path));
        const size = assets.reduce((sum, item) => sum + (item.gzip || item.raw || 0), 0);
        const files = assets.filter((item) => item.hasGzip).length;
        if (assets.length !== allocation.assets.length) {
            issues.push(`${allocation.capability} lazy asset set is incomplete`);
        }
        if (size > allocation.lazyGzip) {
            issues.push(`${allocation.capability} lazy gzip ${formatBytes(size)} exceeds ${formatBytes(allocation.lazyGzip)}`);
        }
        if (files > allocation.gzipFiles) {
            issues.push(`${allocation.capability} lazy gzip file count ${files} exceeds ${allocation.gzipFiles}`);
        }
        if ((report.boot.files || []).some((asset) => allocation.assets.includes(asset))) {
            issues.push(`${allocation.capability} assets must remain outside the boot resource set`);
        }
        lazyGzip += size;
        lazyGzipLimit += allocation.lazyGzip;
        lazyFiles += files;
        lazyFileLimit += allocation.gzipFiles;
    });
    // Unused lazy-page space cannot hide growth in unrelated/shared assets.
    if (lazyGzipLimit && report.totals.gzip - lazyGzip > budgets.totalGzip - lazyGzipLimit) {
        issues.push(`base/shared gzip ${formatBytes(report.totals.gzip - lazyGzip)} exceeds ` +
            `${formatBytes(budgets.totalGzip - lazyGzipLimit)}`);
    }
    if (lazyFileLimit && gzipFiles - lazyFiles > budgets.gzipFiles - lazyFileLimit) {
        issues.push(`base gzip file count ${gzipFiles - lazyFiles} exceeds ${budgets.gzipFiles - lazyFileLimit}`);
    }
    return issues;
}

function findHtmlTagEnd(source, start) {
    let quote = '';
    for (let index = start; index < source.length; index += 1) {
        const char = source[index];
        if (quote) {
            if (char === quote) quote = '';
            continue;
        }
        if (char === '"' || char === "'") {
            quote = char;
        } else if (char === '>') {
            return index;
        }
    }
    return -1;
}

// Count markup elements without executing the page. Script/style text is raw
// text in HTML and may contain strings such as "<div>"; skip it so the budget
// tracks actual static DOM rather than JavaScript templates.
function countStaticHtmlNodes(source) {
    const html = String(source || '');
    let count = 0;
    let index = 0;
    while (index < html.length) {
        const start = html.indexOf('<', index);
        if (start < 0) break;
        if (html.startsWith('<!--', start)) {
            const commentEnd = html.indexOf('-->', start + 4);
            index = commentEnd < 0 ? html.length : commentEnd + 3;
            continue;
        }
        const end = findHtmlTagEnd(html, start + 1);
        if (end < 0) break;
        const opening = html.slice(start, end + 1);
        const match = opening.match(/^<\s*([A-Za-z][\w:-]*)\b/);
        if (!match) {
            index = end + 1;
            continue;
        }
        count += 1;
        const tagName = match[1].toLowerCase();
        if ((tagName === 'script' || tagName === 'style') && !/\/\s*>$/.test(opening)) {
            const closePattern = new RegExp(`<\\/\\s*${tagName}\\s*>`, 'ig');
            closePattern.lastIndex = end + 1;
            const close = closePattern.exec(html);
            index = close ? closePattern.lastIndex : end + 1;
        } else {
            index = end + 1;
        }
    }
    return count;
}

function collectPublishedHtmlDomStats(wwwDir = WWW_DIR) {
    const files = collectPublishedLogicalResources(wwwDir)
        .filter((relPath) => relPath.endsWith('.html'))
        .map((relPath) => {
            const rawPath = path.join(wwwDir, relPath);
            const gzipPath = `${rawPath}.gz`;
            let content;
            if (fs.existsSync(rawPath)) {
                content = fs.readFileSync(rawPath, 'utf8');
            } else if (fs.existsSync(gzipPath)) {
                content = zlib.gunzipSync(fs.readFileSync(gzipPath)).toString('utf8');
            } else {
                throw new Error(`published HTML is missing: ${relPath}`);
            }
            return { path: relPath, nodes: countStaticHtmlNodes(content) };
        })
        .sort((left, right) => left.path.localeCompare(right.path));
    return {
        files,
        totalNodes: files.reduce((sum, item) => sum + item.nodes, 0),
        maxNodes: files.reduce((max, item) => Math.max(max, item.nodes), 0)
    };
}

function evaluateStaticHtmlDomStats(stats, budgets = DEFAULT_WEB_ASSET_BUDGETS) {
    const issues = [];
    if (!stats || !Array.isArray(stats.files)) return ['Static HTML DOM report is incomplete'];
    if (stats.totalNodes > budgets.staticHtmlNodesTotal) {
        issues.push(`static HTML DOM nodes ${stats.totalNodes} exceed ${budgets.staticHtmlNodesTotal}`);
    }
    const oversized = stats.files.filter((item) => item.nodes > budgets.singleHtmlNodes);
    if (oversized.length > 0) {
        issues.push(`oversized HTML DOM file(s): ${oversized.map((item) =>
            `${item.path}=${item.nodes}`).join(', ')}`);
    }
    let lazyNodes = 0;
    let lazyNodeLimit = 0;
    (budgets.capabilityAllocations || []).forEach((allocation) => {
        const nodes = stats.files.filter((item) => allocation.assets.includes(item.path))
            .reduce((sum, item) => sum + item.nodes, 0);
        if (nodes > allocation.staticHtmlNodesTotal) {
            issues.push(`${allocation.capability} static HTML DOM nodes ${nodes} exceed ${allocation.staticHtmlNodesTotal}`);
        }
        lazyNodes += nodes;
        lazyNodeLimit += allocation.staticHtmlNodesTotal;
    });
    if (lazyNodeLimit && stats.totalNodes - lazyNodes > budgets.staticHtmlNodesTotal - lazyNodeLimit) {
        issues.push(`base static HTML DOM nodes ${stats.totalNodes - lazyNodes} exceed ` +
            `${budgets.staticHtmlNodesTotal - lazyNodeLimit}`);
    }
    return issues;
}

function collectPublishedLogicalResources(wwwDir = WWW_DIR) {
    const resources = new Set();
    walkFiles(wwwDir).forEach((filePath) => {
        let relPath = normalizeRel(path.relative(wwwDir, filePath));
        if (relPath === RESOURCE_MANIFEST_NAME) return;
        if (relPath.endsWith('.gz')) relPath = relPath.slice(0, -3);
        resources.add(relPath);
    });
    return Array.from(resources).sort();
}

function validateResourceManifest(wwwDir = WWW_DIR, expectedContext = null) {
    const issues = [];
    const manifestPath = path.join(wwwDir, RESOURCE_MANIFEST_NAME);
    if (!fs.existsSync(manifestPath)) return [`missing ${RESOURCE_MANIFEST_NAME}`];

    let manifest;
    try {
        manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8').replace(/^\uFEFF/, ''));
    } catch (error) {
        return [`invalid ${RESOURCE_MANIFEST_NAME}: ${error.message}`];
    }

    if (manifest.schemaVersion !== 1) issues.push('resource manifest schemaVersion must be 1');
    if (manifest.runtimeConfigurationAffectsResources !== false) {
        issues.push('runtime configuration must not affect compiled Web resources');
    }
    if (!manifest.environment || !manifest.capabilityFingerprint || !manifest.compiledCapabilities) {
        issues.push('resource manifest is missing environment/capability metadata');
    }
    if (expectedContext) {
        if (manifest.environment !== expectedContext.environment) {
            issues.push(`manifest environment ${manifest.environment} does not match ${expectedContext.environment}`);
        }
        if (manifest.capabilityFingerprint !== expectedContext.capabilityFingerprint) {
            issues.push('manifest capability fingerprint does not match the active build context');
        }
    }

    const declared = Array.isArray(manifest.resources)
        ? manifest.resources.map(normalizeRel).sort()
        : [];
    const actual = collectPublishedLogicalResources(wwwDir);
    if (JSON.stringify(declared) !== JSON.stringify(actual)) {
        const declaredSet = new Set(declared);
        const actualSet = new Set(actual);
        const missing = declared.filter((item) => !actualSet.has(item));
        const undeclared = actual.filter((item) => !declaredSet.has(item));
        if (missing.length > 0) issues.push(`manifest resource(s) missing on disk: ${missing.join(', ')}`);
        if (undeclared.length > 0) issues.push(`published resource(s) missing from manifest: ${undeclared.join(', ')}`);
    }

    const manifestContext = { compiledCapabilities: manifest.compiledCapabilities || {} };
    Object.keys(ASSET_CAPABILITY_REQUIREMENTS).sort().forEach((relPath) => {
        const expected = shouldIncludeAsset(relPath, manifestContext);
        const published = declared.includes(relPath);
        if (expected !== published) {
            issues.push(`${relPath} must be ${expected ? 'included' : 'excluded'} for compiled capabilities`);
        }
    });
    return issues;
}

function sliceMethod(source, marker, nextMarker) {
    const start = source.indexOf(marker);
    if (start < 0) return '';
    const end = nextMarker ? source.indexOf(nextMarker, start + marker.length) : -1;
    return source.slice(start, end < 0 ? source.length : end);
}

function validateLifecycleContracts(rootDir = ROOT_DIR) {
    const issues = [];
    const statePath = path.join(rootDir, 'web-src', 'js', 'state.js');
    if (!fs.existsSync(statePath)) return ['missing web-src/js/state.js'];
    const stateSource = fs.readFileSync(statePath, 'utf8');
    const changePage = sliceMethod(stateSource, 'async changePage(page)', '\n    _getPageModuleMap()');
    const hideIndex = changePage.indexOf("_firePageLifecycle(this._pageHideHandlers, 'page-hide')");
    const loadIndex = changePage.indexOf('await this.loadPage(');
    const currentIndex = changePage.indexOf('this.currentPage = normalizedPage');
    const showIndex = changePage.indexOf("_firePageLifecycle(this._pageShowHandlers, 'page-show')");
    if (!(hideIndex >= 0 && loadIndex > hideIndex && currentIndex > loadIndex && showIndex > currentIndex)) {
        issues.push('changePage must hide the old page before loading and show the new page after currentPage changes');
    }

    [
        ['onPageHide(name, fn)', '_pageHideHandlers'],
        ['onPageShow(name, fn)', '_pageShowHandlers']
    ].forEach(([marker, collection]) => {
        const method = sliceMethod(stateSource, marker, '\n    },');
        const replaceIndex = method.indexOf(`${collection}.filter(`);
        const appendIndex = method.indexOf(`${collection}.push(`);
        if (!(replaceIndex >= 0 && appendIndex > replaceIndex && method.includes('h.name !== name'))) {
            issues.push(`${marker} must replace a same-name lifecycle handler before registering it`);
        }
    });

    WEB_SOURCE_DIRS.map((directory) => path.relative(ROOT_DIR, directory)).forEach((relativeDir) => {
        const directory = path.join(rootDir, relativeDir);
        walkFiles(directory).filter((filePath) => filePath.endsWith('.js')).forEach((filePath) => {
            const source = fs.readFileSync(filePath, 'utf8');
            const relPath = normalizeRel(path.relative(rootDir, filePath));
            if (/\bsetInterval\s*\(/.test(source) && !/\bclearInterval\s*\(/.test(source)) {
                issues.push(`${relPath} creates an interval without a clearInterval cleanup path`);
            }
            if (/\bnew\s+EventSource\s*\(/.test(source) && !/\.close\s*\(/.test(source)) {
                issues.push(`${relPath} creates an EventSource without a close cleanup path`);
            }
        });
    });
    return issues;
}

function runWebQualityGate(options = {}) {
    const report = options.report || createWebAssetReport({ topN: 9999 });
    const domStats = options.domStats || collectPublishedHtmlDomStats(options.wwwDir || WWW_DIR);
    let manifest = null;
    try { manifest = readWebResourceManifest(options.wwwDir || WWW_DIR); } catch (_) {
        // validateResourceManifest below supplies the actionable parse error.
    }
    const budgets = options.budgets || resolveWebAssetBudgets(manifest);
    const issues = []
        .concat(evaluateWebAssetReport(report, budgets))
        .concat(evaluateStaticHtmlDomStats(domStats, budgets))
        .concat(validateResourceManifest(options.wwwDir || WWW_DIR, options.context || null))
        .concat(validateLifecycleContracts(options.rootDir || ROOT_DIR));
    if (issues.length > 0) {
        throw new Error(`Web quality gate failed:\n- ${issues.join('\n- ')}`);
    }
    console.log(
        `Web quality gate OK: total=${formatBytes(report.totals.gzip)}/${formatBytes(budgets.totalGzip)}, ` +
        `boot=${formatBytes(report.boot.gzip)}, assets=${report.totals.files}, ` +
        `gzipFiles=${(report.assets || []).filter((item) => item.hasGzip).length}, ` +
        `staticDom=${domStats.totalNodes}, ` +
        `capabilityBudgets=${(budgets.capabilityAllocations || []).map((item) => item.capability).join(',') || 'base'}`
    );
    return { report, domStats, budgets, issues };
}

if (require.main === module) {
    try {
        runWebQualityGate();
    } catch (error) {
        console.error(error.message || error);
        process.exitCode = 1;
    }
}

module.exports = {
    CAPABILITY_WEB_ASSET_BUDGETS,
    DEFAULT_WEB_ASSET_BUDGETS,
    collectPublishedHtmlDomStats,
    collectPublishedLogicalResources,
    countStaticHtmlNodes,
    evaluateStaticHtmlDomStats,
    evaluateWebAssetReport,
    resolveWebAssetBudgets,
    runWebQualityGate,
    validateLifecycleContracts,
    validateResourceManifest
};
