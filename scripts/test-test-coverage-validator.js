'use strict';

const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');

const {
    analyzeTestSource,
    CRITICAL_REGRESSION_CONTRACTS,
    extractCiFirmwareEnvironments,
    listTestFiles,
    maskCppNonCode,
    validateCriticalRegressionContracts,
    validateTestAnalysis
} = require('./validate-test-coverage');

const fixture = String.raw`
static void test_real_case() {
    const char *text = "RUN_TEST(test_string_case) { }";
    const char marker = '}';
    const char *raw = u8R"tag(RUN_TEST(test_raw_case) { })tag";
}

// RUN_TEST(test_line_comment)
/* RUN_TEST(test_block_comment) */
void test_fixture_group() {
    RUN_TEST(test_real_case);
}
`;

const masked = maskCppNonCode(fixture);
assert.ok(!masked.includes('test_string_case'));
assert.ok(!masked.includes('test_raw_case'));
assert.ok(!masked.includes('test_line_comment'));
assert.ok(!masked.includes('test_block_comment'));

const analysis = analyzeTestSource(fixture);
assert.deepStrictEqual(analysis.groups.map((item) => item.name), ['test_fixture_group']);
assert.deepStrictEqual(analysis.tests.map((item) => item.name), ['test_real_case']);
assert.deepStrictEqual(analysis.registrations.map((item) => [item.name, item.container]), [
    ['test_real_case', 'test_fixture_group']
]);

const issues = [];
validateTestAnalysis(
    fixture,
    analysis,
    'fixture.cpp',
    new Set(['test_fixture_group']),
    new Set(['test_fixture_group']),
    issues,
    { files: 0, groups: 0, tests: 0 },
    new Map()
);
assert.deepStrictEqual(issues, []);

const invalid = `${fixture}\nRUN_TEST(test_real_case);\n`;
const invalidIssues = [];
validateTestAnalysis(
    invalid,
    analyzeTestSource(invalid),
    'invalid.cpp',
    new Set(['test_fixture_group']),
    new Set(['test_fixture_group']),
    invalidIssues,
    { files: 0, groups: 0, tests: 0 },
    new Map()
);
assert.ok(invalidIssues.some((issue) => issue.includes('duplicate RUN_TEST')));
assert.ok(invalidIssues.some((issue) => issue.includes('must be inside')));

const ciFixture = `jobs:
  firmware-build:
    strategy:
      matrix:
        environment:
          - esp32-F4R0
          - esp32c3-F4R0
  other-job:
    runs-on: ubuntu-latest
`;
assert.deepStrictEqual(extractCiFirmwareEnvironments(ciFixture), ['esp32-F4R0', 'esp32c3-F4R0']);

const criticalIssues = [];
validateCriticalRegressionContracts(criticalIssues);
assert.deepStrictEqual(criticalIssues, []);
assert.equal(CRITICAL_REGRESSION_CONTRACTS.length, 13);

const peripheralWorkspaceMarker = 'def test_workspace_only_exposes_peripheral_configuration';
const missingPeripheralWorkspaceIssues = [];
validateCriticalRegressionContracts(missingPeripheralWorkspaceIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/unit/test_p1_peripheral_web_contract.py'
        ? source.replace(peripheralWorkspaceMarker, 'def removed_peripheral_workspace_scope_contract')
        : source;
});
assert.ok(missingPeripheralWorkspaceIssues.some((issue) =>
    issue.includes('peripheral configuration workspace scope') && issue.includes(peripheralWorkspaceMarker)));

const missingPeripheralWorkspaceFileIssues = [];
validateCriticalRegressionContracts(missingPeripheralWorkspaceFileIssues, (rel) => {
    if (rel === 'test/unit/test_p1_peripheral_web_contract.py') throw new Error('not found');
    return fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
});
assert.ok(missingPeripheralWorkspaceFileIssues.some((issue) =>
    issue.includes('peripheral configuration workspace scope') &&
    issue.includes('missing test/unit/test_p1_peripheral_web_contract.py')));

const factoryResetMarker = 'def test_web_reset_reports_partial_failure_without_scheduling_reboot';
const missingFactoryResetIssues = [];
validateCriticalRegressionContracts(missingFactoryResetIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/unit/test_factory_reset_persistence_contract.py'
        ? source.replace(factoryResetMarker, 'def removed_factory_reset_failure_contract')
        : source;
});
assert.ok(missingFactoryResetIssues.some((issue) =>
    issue.includes('feature persistence and factory reset durability') &&
    issue.includes(factoryResetMarker)));

const removedMarker = 'def test_ai_conversion_uses_guarded_linear_mapping';
const missingCriticalIssues = [];
validateCriticalRegressionContracts(missingCriticalIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/unit/test_analog_input_contract.py'
        ? source.replace(removedMarker, 'def removed_ai_conversion_contract')
        : source;
});
assert.ok(missingCriticalIssues.some((issue) =>
    issue.includes('analog and digital input conversion') && issue.includes(removedMarker)));

const mqttLoadingMarker = 'MQTT-003B: 首次状态返回前显示中性检测态 @quick';
const missingMqttLoadingIssues = [];
validateCriticalRegressionContracts(missingMqttLoadingIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/browser/suites/25-mqtt-status-feedback.spec.ts'
        ? source.replace(mqttLoadingMarker, 'removed MQTT neutral loading contract')
        : source;
});
assert.ok(missingMqttLoadingIssues.some((issue) =>
    issue.includes('mqtt failure feedback') && issue.includes(mqttLoadingMarker)));

const mqttSequenceMarker = 'MQTT-003D: 旧状态响应不得覆盖新页面结果 @quick';
const missingMqttSequenceIssues = [];
validateCriticalRegressionContracts(missingMqttSequenceIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/browser/suites/25-mqtt-status-feedback.spec.ts'
        ? source.replace(mqttSequenceMarker, 'removed MQTT stale response contract')
        : source;
});
assert.ok(missingMqttSequenceIssues.some((issue) =>
    issue.includes('mqtt failure feedback') && issue.includes(mqttSequenceMarker)));

const mobileModalMarker = 'UI-007D: 360px 长动态内容模态框无横向溢出 @quick';
const missingWebInteractionIssues = [];
validateCriticalRegressionContracts(missingWebInteractionIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/browser/suites/15-ui-regression.spec.ts'
        ? source.replace(mobileModalMarker, 'removed mobile modal overflow contract')
        : source;
});
assert.ok(missingWebInteractionIssues.some((issue) =>
    issue.includes('web interaction quality') && issue.includes(mobileModalMarker)));

const staticIntegrityMarker = 'void test_page_loader_response_integrity_locked()';
const missingStaticIntegrityIssues = [];
validateCriticalRegressionContracts(missingStaticIntegrityIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/test_static_concurrency.cpp'
        ? source.replace(staticIntegrityMarker, 'void removed_page_loader_integrity_contract()')
        : source;
});
assert.ok(missingStaticIntegrityIssues.some((issue) =>
    issue.includes('static resource transport integrity') && issue.includes(staticIntegrityMarker)));

const c3AdaptiveLimiterMarker = 'void test_limiter_no_psram_adaptive_behavior()';
const missingC3AdaptiveLimiterIssues = [];
validateCriticalRegressionContracts(missingC3AdaptiveLimiterIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/test_static_concurrency.cpp'
        ? source.replace(c3AdaptiveLimiterMarker, 'void removed_c3_adaptive_limiter_contract()')
        : source;
});
assert.ok(missingC3AdaptiveLimiterIssues.some((issue) =>
    issue.includes('static resource transport integrity') && issue.includes(c3AdaptiveLimiterMarker)));

const staticLeaseMarkers = [
    'void test_static_transfer_active_reaps_expired_leases()',
    'void test_limiter_millis_wraparound_safe()',
    'void test_limiter_staggered_expiry_isolated()',
    'void test_limiter_stale_disconnect_generation_safe()'
];
for (const marker of staticLeaseMarkers) {
    const missingStaticLeaseIssues = [];
    validateCriticalRegressionContracts(missingStaticLeaseIssues, (rel) => {
        const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
        return rel === 'test/test_static_concurrency.cpp'
            ? source.replace(marker, 'void removed_static_lease_contract()')
            : source;
    });
    assert.ok(missingStaticLeaseIssues.some((issue) =>
        issue.includes('static resource transport integrity') && issue.includes(marker)));
}

const webP95GateMarker = "durationLimitFromEnv('WEB_ROOT_P95_MAX_MS', 8_000)";
const missingWebP95GateIssues = [];
validateCriticalRegressionContracts(missingWebP95GateIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/browser/suites/24-web-smooth-access.spec.ts'
        ? source.replace(webP95GateMarker, "durationLimitFromEnv('WEB_ROOT_P95_MAX_MS', 15_000)")
        : source;
});
assert.ok(missingWebP95GateIssues.some((issue) =>
    issue.includes('static resource transport integrity') && issue.includes(webP95GateMarker)));

const loginDiagnosticMarker = 'def test_browser_login_failure_diagnostics_are_attached_and_redacted';
const missingLoginDiagnosticIssues = [];
validateCriticalRegressionContracts(missingLoginDiagnosticIssues, (rel) => {
    const source = fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
    return rel === 'test/unit/test_auth_session_contract.py'
        ? source.replace(loginDiagnosticMarker, 'def removed_browser_login_failure_diagnostics')
        : source;
});
assert.ok(missingLoginDiagnosticIssues.some((issue) =>
    issue.includes('authentication session reuse and safety') && issue.includes(loginDiagnosticMarker)));

const missingFileIssues = [];
validateCriticalRegressionContracts(missingFileIssues, (rel) => {
    if (rel === 'test/unit/test_auth_session_contract.py') throw new Error('not found');
    return fs.readFileSync(path.join(__dirname, '..', rel), 'utf8');
});
assert.ok(missingFileIssues.some((issue) =>
    issue.includes('authentication session reuse and safety') && issue.includes('missing test/unit/test_auth_session_contract.py')));

const tempRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'fastbee-test-discovery-'));
try {
    fs.mkdirSync(path.join(tempRoot, 'nested'));
    fs.mkdirSync(path.join(tempRoot, 'fixtures'));
    fs.writeFileSync(path.join(tempRoot, 'test_top.cpp'), fixture);
    fs.writeFileSync(path.join(tempRoot, 'nested', 'test_nested.cpp'), fixture);
    fs.writeFileSync(path.join(tempRoot, 'fixtures', 'test_ignored.cpp'), fixture);
    fs.writeFileSync(path.join(tempRoot, 'Test_wrong_case.cpp'), fixture);
    const discoveryIssues = [];
    const files = listTestFiles(tempRoot, discoveryIssues);
    assert.deepStrictEqual(files.map((file) => path.basename(file)).sort(), ['test_nested.cpp', 'test_top.cpp']);
    assert.ok(discoveryIssues.some((issue) => issue.includes('lowercase test_ prefix')));
} finally {
    fs.rmSync(tempRoot, { recursive: true, force: true });
}

console.log('FastBee test coverage validator contract OK');
