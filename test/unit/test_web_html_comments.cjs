'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const {createRequire} = require('node:module');
const {chromium} = require('../browser/node_modules/playwright');
const {stripReviewedHtmlComments: strip} = require('../../scripts/web-html-comments');
const root = path.resolve(__dirname, '../..');
const fragment = 'pages/modals.html';
const reviewedFragments = [
    'pages/modals.html', 'pages/network.html', 'pages/admin.html',
    'pages/features.html', 'pages/peripheral.html', 'pages/protocol.html',
    'pages/rule-script.html', 'pages/fragments/device-ota.html',
    'pages/fragments/protocol-modbus-rtu.html'
];

const text = '\n  <!-- description -->\n<button title="a > <!--keep-->" data-label=\'<!--also keep-->\'> x </button>\n';
assert.equal(strip(fragment, text), text.replace('<!-- description -->', ''));
assert.equal(strip('pages/other.html', text), text);
assert.equal(strip(fragment, '<!--[if IE]>legacy<![endif]--><!-- ko if: ok --><!-- /ko -->'),
    '<!--[if IE]>legacy<![endif]--><!-- ko if: ok --><!-- /ko -->');
for (const tag of ['script','style','textarea','title','xmp','iframe','noembed','noframes','plaintext','svg','math']) {
    const html = '<!-- keep all --><' + tag + '>literal <!-- content --></' + tag + '>';
    assert.equal(strip(fragment, html), html, tag);
}
for (const html of ['<!-- incomplete', '<!--valid--><p title="unfinished', '<!--valid--><!DOCTYPE html>', '<!--valid--><?xml ?>'])
    assert.equal(strip(fragment, html), html);

function captureWriter(removeComments) {
    const script = path.join(root, 'scripts/build-web-modules.js'), nativeRequire = createRequire(script);
    const publishRoot = path.join(root, 'data/www'), output = new Map();
    const virtualPath = file => {
        const relative = path.relative(publishRoot, path.resolve(file));
        assert.ok(!relative.startsWith('..') && !path.isAbsolute(relative));
        return relative.replace(/\\/g, '/');
    };
    const captureFs = new Proxy(fs, {get(target, key) {
        if (key === 'writeFileSync') return (file, content) => output.set(virtualPath(file), String(content));
        if (key === 'mkdirSync') return file => {virtualPath(file);};
        if (key === 'existsSync') return file => path.resolve(file).startsWith(publishRoot + path.sep)
            ? output.has(virtualPath(file)) : target.existsSync(file);
        if (/^(?:write|append|unlink|rm|rmdir|rename|chmod|chown|truncate|copy|cp|symlink|link|createWriteStream)/.test(String(key)))
            return () => {throw new Error('Unexpected shared mutation: ' + key);};
        return target[key];
    }});
    const mod = {exports: {}};
    const wrapped = vm.runInThisContext('(function(require,module,exports,__filename,__dirname){\n' + fs.readFileSync(script, 'utf8') + '\n})', {filename: script});
    wrapped(name => name === 'fs' ? captureFs : name === './web-html-comments' && !removeComments
        ? {stripReviewedHtmlComments: (_, source) => source} : nativeRequire(name),
        mod, mod.exports, script, path.dirname(script));
    mod.exports.syncStaticAssets();
    return output;
}

(async () => {
    const original = captureWriter(false), published = captureWriter(true);
    assert.deepEqual([...published.keys()], [...original.keys()]);
    const rows = [];
    for (const [name, html] of original) {
        const optimized = published.get(name);
        if (!reviewedFragments.includes(name)) assert.equal(optimized, html, name);
        else {
            assert.ok(html.includes('<!--') && optimized.length < html.length, 'real writer did not remove comments: ' + name);
            assert.equal(optimized.replace(/<!--[\s\S]*?-->/g, ''), html.replace(/<!--[\s\S]*?-->/g, ''),
                'changed non-comment bytes: ' + name);
            rows.push({name, html, optimized});
        }
    }
    assert.deepEqual(rows.map(row => row.name).sort(), [...reviewedFragments].sort());
    const browser = await chromium.launch({headless: true});
    try {
        const page = await browser.newPage();
        const snapshots = await page.evaluate(rows => rows.map(({name, html, optimized}) => {
            const snapshot = source => {
                const document = new DOMParser().parseFromString(source, 'text/html');
                const walk = node => {
                    if (node.nodeType === Node.COMMENT_NODE) return null;
                    if (node.nodeType === Node.TEXT_NODE) return ['text', node.data];
                    if (node.nodeType !== Node.ELEMENT_NODE) return [node.nodeName];
                    const children = [...node.childNodes].map(walk).filter(Boolean);
                    // Removed comments can merge adjacent text nodes; compare
                    // their concatenated text without discarding any whitespace.
                    const merged = [];
                    for (const child of children) {
                        if (child[0] === 'text' && merged.at(-1)?.[0] === 'text') merged.at(-1)[1] += child[1];
                        else merged.push(child);
                    }
                    return [node.localName, [...node.attributes].map(a => [a.name, a.value]),
                        'value' in node ? node.value : null, 'checked' in node ? node.checked : null, merged];
                };
                return walk(document.documentElement);
            };
            return {name, before: snapshot(html), after: snapshot(optimized)};
        }), rows);
        for (const row of snapshots) assert.deepEqual(row.after, row.before, 'browser DOM/form drift: ' + row.name);

        // Exercise the existing consumers as well: real modal caching/mounting,
        // close handlers and network form/mode/DHCP bindings against both bytes.
        // API/save endpoints alone are fixtures; no transport or device is used.
        await page.evaluate(() => {
            window.AppState = {registerModule(_, members) {Object.assign(this, members);}};
            window.apiGet = async () => ({success: true, data: {ethernet: true, cellular: true, supported: false}});
            window.fetch = () => {throw new Error('Unexpected network request');};
        });
        for (const script of ['web-src/js/page-loader.js','web-src/js/state-ui.js','web-src/modules/runtime/network.js'])
            await page.addScriptTag({content: fs.readFileSync(path.join(root, script), 'utf8')});
        const interactions = await page.evaluate(rows => {
            const results = [];
            for (const variant of ['html','optimized']) {
                document.body.innerHTML = rows.find(row => row.name === 'pages/network.html')[variant];
                PageLoader._modalTemplates = {}; PageLoader._modalHosts = {};
                PageLoader._cacheModalTemplates(rows.find(row => row.name === 'pages/modals.html')[variant], '',
                    ['change-password-modal', 'peripheral-modal']);
                const modalIds = Object.keys(PageLoader._modalTemplates).sort(), closed = [];
                for (let cycle = 0; cycle < 2; cycle += 1) for (const id of modalIds) {
                    const modal = PageLoader.mountModal(id);
                    AppState.showModal(modal);
                    if (modal.style.display !== 'flex') throw new Error('Modal did not open: ' + id);
                    const close = modal.querySelector('.modal-close, .modal-close-btn');
                    if (!close) throw new Error('Missing close button: ' + id);
                    close.click();
                    closed.push(document.getElementById(id) === null);
                }
                const saves = [];
                for (const method of ['saveNetworkConfig','saveAPConfig','saveAdvancedConfig','saveEthernetConfig','saveCellularConfig'])
                    AppState[method] = () => saves.push(method);
                AppState.setupNetworkEvents();
                AppState.setupNetworkEvents(); // Existing bind-once guard must survive.
                const prevented = ['wifi-form','ap-form','advanced-form','ethernet-form','cellular-form'].map(id =>
                    !document.getElementById(id).dispatchEvent(new Event('submit', {bubbles: true, cancelable: true})));
                const type = document.getElementById('network-type');
                const ipTitle = document.querySelector('#advanced-form > .fb-section-title');
                const connTitle = [...document.querySelectorAll('#advance .fb-section-title-mt')]
                    .find(node => !node.closest('#domain-config-section'));
                const modes = ['2', '0', '1', '2'].map(value => {
                    type.value = value; type.dispatchEvent(new Event('change', {bubbles: true}));
                    clearTimeout(AppState._networkModeChangeTimer); AppState._networkModeChangeTimer = null;
                    return {
                        panels: ['wifi-panel','ethernet-panel','cellular-panel'].map(id => document.getElementById(id).style.display),
                        ipTitle: ipTitle.style.display, ipGrid: ipTitle.nextElementSibling.style.display,
                        connectionGrid: connTitle.nextElementSibling.style.display
                    };
                });
                const dhcp = document.getElementById('wifi-dhcp'), group = document.getElementById('static-ip').closest('.fb-form-group');
                dhcp.value = '1'; dhcp.dispatchEvent(new Event('change', {bubbles: true}));
                const staticVisible = group.style.display;
                dhcp.value = '0'; dhcp.dispatchEvent(new Event('change', {bubbles: true}));
                results.push({modalIds, closed, saves, prevented, modes, staticVisible, dynamicHidden: group.style.display});
            }
            return results;
        }, rows);
        assert.deepEqual(interactions[1], interactions[0], 'real modal/network consumer interaction drift');
        assert.equal(interactions[1].modalIds.length, 4);
        assert.ok(interactions[1].closed.every(Boolean));
        assert.ok(interactions[1].prevented.every(Boolean));
        assert.deepEqual(interactions[1].saves,
            ['saveNetworkConfig','saveAPConfig','saveAdvancedConfig','saveEthernetConfig','saveCellularConfig']);
        assert.deepEqual(interactions[1].modes, [
            {panels:['none','none','block'], ipTitle:'none', ipGrid:'none', connectionGrid:'none'},
            {panels:['block','none','none'], ipTitle:'', ipGrid:'', connectionGrid:''},
            {panels:['none','block','none'], ipTitle:'', ipGrid:'', connectionGrid:'none'},
            {panels:['none','none','block'], ipTitle:'none', ipGrid:'none', connectionGrid:'none'}
        ]);
        assert.equal(interactions[1].staticVisible, 'block');
        assert.equal(interactions[1].dynamicHidden, 'none');
    } finally {await browser.close();}
    console.log('PASS reviewed HTML writer comments: exact non-comment bytes, DOM/form equivalence and real modal/network interactions');
})().catch(error => {console.error(error);process.exitCode = 1;});
