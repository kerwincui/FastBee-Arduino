'use strict';

/**
 * C1: test:static 静态检查清单（唯一事实源）。
 *
 * package.json 的 test:static 只保留 `node scripts/run-static-checks.js`，
 * 由 scripts/run-static-checks.js 按本清单顺序逐项执行。
 * 新增、下线或调整静态检查时只改这里，数组顺序即执行顺序，
 * 最后一项固定为 git diff --check。
 */
module.exports = [
    {
        name: 'check-utf8-text',
        command: 'node scripts/check-utf8-text.js README.md README.en.md docs scripts test web-src include src data/config/peripherals.json data/config/periph_exec.json data/config/users.json'
    },
    { name: 'validate-test-coverage', command: 'node scripts/validate-test-coverage.js' },
    { name: 'validate-device-api-matrix', command: 'node scripts/validate-device-api-matrix.js' },
    { name: 'validate-device-config-examples', command: 'node scripts/validate-device-config-examples.js' },
    { name: 'validate-mqtt-ntp-lifecycle', command: 'node scripts/validate-mqtt-ntp-lifecycle.js' },
    { name: 'validate-build-matrix', command: 'node scripts/validate-build-matrix.js' },
    { name: 'validate-doc-links', command: 'node scripts/validate-doc-links.js' },
    { name: 'validate-stability-thresholds', command: 'node scripts/validate-stability-thresholds.js' },
    { name: 'validate-config-defaults', command: 'node scripts/validate-config-defaults.js' },
    { name: 'validate-i18n', command: 'node scripts/validate-i18n.js' },
    { name: 'validate-file-sizes', command: 'node scripts/validate-file-sizes.js' },
    { name: 'validate-rule-script-profile', command: 'node scripts/validate-rule-script-profile.js' },
    // 该脚本无 shebang 且仓库以 node 调用（见 HEAD package.json 的 test:static 链），故保留 node 前缀。
    { name: 'validate-libdeps-patches', command: 'node scripts/validate-libdeps-patches.js' },
    { name: 'validate-feature-budgets', command: 'node scripts/validate-feature-budgets.js' },
    { name: 'validate-project-hygiene', command: 'node scripts/validate-project-hygiene.js' },
    { name: 'test-fim-manifest-validator', command: 'node scripts/test-fim-manifest-validator.js' },
    { name: 'validate-generic-architecture', command: 'node scripts/validate-generic-architecture.js' },
    { name: 'validate-network-ui', command: 'node scripts/validate-network-ui.js' },
    { name: 'test-web-assets', command: 'npm run test:web-assets' },
    { name: 'test-meta', command: 'npm run test:meta' },
    { name: 'git-diff-check', command: 'git diff --check' }
];
