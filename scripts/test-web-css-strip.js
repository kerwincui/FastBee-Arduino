'use strict';

// B7 stage 1: verify unused-CSS-rule stripping is conservative and auditable.
// Covers the fixture-level invariants (only fully unreferenced selector lists
// are removed, dynamic class prefixes keep rules, mixed selector lists stay
// whole) and the real main.css against the publish reference corpus.

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const {
    stripUnreferencedCssRules,
    getCssReferenceCorpus,
    minifyCSS
} = require('./build-web-modules');

const ROOT_DIR = path.resolve(__dirname, '..');

function countChars(text, char) {
    return text.split('').filter((item) => item === char).length;
}

function assertBalancedBraces(text, label) {
    assert.strictEqual(countChars(text, '{'), countChars(text, '}'), `${label}: unbalanced braces`);
}

function runFixtureTests() {
    const corpus = {
        text: [
            '<div class="used-class spinner">',
            '<input type="text">',
            "const cls = 'dyn-' + kind;",
            'el.classList.add("used-class");'
        ].join('\n'),
        dynamicPrefixes: ['dyn-']
    };
    const fixture = [
        '/* header comment */',
        '.used-class { color: red; }',
        '.dead-class { color: blue; }',
        'input[type="text"], .dead-mixed { margin: 0; }',
        '.dead-a, .dead-b { padding: 0; }',
        '[data-theme="dark"] .dead-a { color: #000; }',
        '.dyn-anything { display: block; }',
        'a[href="#top"] { text-decoration: none; }',
        '#dead-id { border: 0; }',
        '@media (max-width: 600px) { .dead-media { color: red; } }',
        '@media (max-width: 800px) { .used-class { font-size: 1em; } .dead-media2 { color: red; } }',
        '@keyframes deadSpin { from { opacity: 0; } }',
        '@keyframes liveSpin { from { opacity: 1; } }',
        '.spinner { animation: liveSpin 1s linear; }'
    ].join('\n');

    const result = stripUnreferencedCssRules(fixture, corpus);
    const out = result.css;
    assertBalancedBraces(out, 'fixture output');

    // Kept: referenced classes, dynamic prefixes, element-only selectors,
    // mixed selector lists (stage 1 never trims inside a selector list).
    ['.used-class', '.dyn-anything', '.dead-mixed', 'a[href="#top"]', '.spinner',
        '@media (max-width: 800px)', '@keyframes liveSpin'].forEach((needle) => {
        assert.ok(out.includes(needle), `fixture must keep ${needle}`);
    });
    // Removed: every rule whose anchors are all unreferenced.
    ['.dead-class', '.dead-a', '.dead-b', '#dead-id', '.dead-media',
        '.dead-media2', '@media (max-width: 600px)', '@keyframes deadSpin',
        'header comment'].forEach((needle) => {
        assert.ok(!out.includes(needle), `fixture must strip ${needle}`);
    });

    const kinds = result.removed.map((item) => item.kind);
    assert.strictEqual(kinds.filter((kind) => kind === 'rule').length, 6, 'fixture removed rule count');
    assert.strictEqual(kinds.filter((kind) => kind === 'at-block').length, 1, 'fixture removed media block count');
    assert.strictEqual(kinds.filter((kind) => kind === 'keyframes').length, 1, 'fixture removed keyframes count');

    const minified = minifyCSS(out);
    assertBalancedBraces(minified, 'fixture minified output');
    assert.ok(minified.includes('.used-class'), 'minified fixture keeps live rule');
    assert.ok(!minified.includes('.dead-class'), 'minified fixture drops dead rule');
}

function runMainCssIntegration() {
    const source = fs.readFileSync(path.join(ROOT_DIR, 'web-src', 'css', 'main.css'), 'utf8');
    const corpus = getCssReferenceCorpus();
    assert.ok(corpus.text.length > 100000, 'reference corpus must cover the published web sources');

    const result = stripUnreferencedCssRules(source, corpus);
    assert.ok(result.removed.length > 0, 'main.css must yield strippable rules');
    assertBalancedBraces(result.css, 'main.css stripped output');

    // Selectors proven referenced by web-src HTML/JS must survive stripping.
    ['.fb-btn', '.file-input-wrapper', '.dev-mode-locked', '.motor-run-badge',
        '.fb-form-group', '@keyframes slideIn'].forEach((needle) => {
        assert.ok(result.css.includes(needle), `stripped main.css must keep ${needle}`);
    });
    // Known dead rules verified unreferenced across all of web-src (2026-10-14).
    ['.weather-test-forecast', '.theme-toggle', '.motor-run-forward',
        '.remote-resource-item', '@keyframes cardAppear'].forEach((needle) => {
        assert.ok(!result.css.includes(needle), `stripped main.css must drop ${needle}`);
    });

    const minified = minifyCSS(result.css);
    assertBalancedBraces(minified, 'main.css minified stripped output');

    console.log(`main.css strip: ${result.removed.length} block(s), ` +
        `${result.removed.reduce((sum, item) => sum + item.bytes, 0)} B raw, ` +
        `${Buffer.byteLength(source, 'utf8')} -> ${Buffer.byteLength(result.css, 'utf8')} B`);
}

runFixtureTests();
runMainCssIntegration();
console.log('Web CSS strip tests OK');
