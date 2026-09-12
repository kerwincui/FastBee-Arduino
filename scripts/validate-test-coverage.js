'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const TEST_DIR = path.join(ROOT_DIR, 'test');
const MAIN_FILE = path.join(TEST_DIR, 'test_main.cpp');
const PACKAGE_FILE = path.join(ROOT_DIR, 'package.json');
const CI_FILE = path.join(ROOT_DIR, '.github', 'workflows', 'ci.yml');
const PLATFORMIO_FILE = path.join(ROOT_DIR, 'platformio.ini');
const UNIT_RUNNER_FILE = path.join(ROOT_DIR, 'scripts', 'run-unit-tests.js');
const TEST_ALL_FILE = path.join(ROOT_DIR, 'scripts', 'test-all.ps1');
const EXCLUDED_TEST_DIRS = new Set(['browser', 'e2e', 'fixtures', 'mocks', 'test_embedded', 'unit']);
const CRITICAL_REGRESSION_CONTRACTS = [
    {
        name: 'feature menu gating',
        files: {
            'test/unit/test_menu_feature_contract.py': [
                'def test_only_optional_pages_are_feature_gated',
                'def test_removed_file_manager_page_has_no_web_route_or_test_project',
                'def test_authenticated_status_requires_compiled_and_active',
                'def test_status_failures_do_not_restore_all_menus',
                'def test_fleet_quick_includes_live_menu_visibility_regression',
                'def test_browser_navigation_rejects_hidden_preloaded_pages'
            ]
        }
    },
    {
        name: 'memory degradation and recovery',
        files: {
            'scripts/test-memory-regressions.js': [
                "setRuntimeDisabled(\"fileLogging\"",
                'PeriphExec must reject heavy event rules during severe memory pressure',
                'MQTTS recovery must fall back to persisted plain MQTT'
            ],
            'test/unit/test_deploy_config_bundle.py': [
                'def test_classic_lite_disables_mqtt_before_staging',
                'def test_mqtt_degradation_is_reported_separately_from_runtime_features',
                'def test_memory_profile_prunes_only_profile_disabled_runtime_entries'
            ],
            'test/test_memory_budget.cpp': [
                'void test_guard_level_uses_internal_dram_budgets()',
                'void test_periph_exec_resource_guard()'
            ]
        }
    },
    {
        name: 'mqtt failure feedback',
        files: {
            'test/unit/test_mqtt_status_visibility.py': [
                'def test_readable_error_message_is_exposed_to_ui',
                'def test_http_503_memory_reason_is_actionable',
                'def test_browser_network_error_is_translated'
            ],
            'test/browser/suites/25-mqtt-status-feedback.spec.ts': [
                'MQTT-003A: 连接失败原因明确显示且不修改配置 @quick'
            ]
        }
    },
    {
        name: 'analog and digital input conversion',
        files: {
            'test/unit/test_analog_input_contract.py': [
                'def test_ai_conversion_fields_are_exposed_and_submitted',
                'def test_ai_conversion_uses_guarded_linear_mapping',
                'def test_di_options_round_trip_and_simple_defaults',
                'def test_di_report_modes_have_distinct_runtime_paths'
            ],
            'test/test_peripheral_manager_opt.cpp': [
                'void test_analog_raw_conversion_boundaries()',
                'void test_analog_output_percent_mapping_uses_production_helper()'
            ],
            'test/browser/suites/05-peripheral.spec.ts': [
                'PER-083: AI和DI参数新增保存、编辑回填及清理闭环'
            ]
        }
    },
    {
        name: 'five-device independent configuration',
        files: {
            'scripts/validate-device-config-examples.js': [
                "environment: 'esp32c3-F4R0'",
                "environment: 'esp32c6-F4R0'",
                "environment: 'esp32-F4R0'",
                "environment: 'esp32s3-F16R8'",
                "environment: 'esp32s3-F8R0'",
                'configuration is identical to'
            ],
            'test/unit/test_deploy_config_bundle.py': [
                'def test_all_five_devices_have_unique_serial_targets',
                'def test_fleet_refuses_identical_configuration_sources',
                'def test_repository_examples_are_the_default_configuration_sources'
            ]
        }
    },
    {
        name: 'configuration transfer roundtrip',
        files: {
            'test/unit/test_config_transfer_contract.py': [
                'def test_live_smoke_imports_the_just_exported_document',
                'def test_import_success_checks_envelope_and_returned_file_name',
                'def test_memory_degradation_requires_retryable_503_metadata'
            ],
            'test/test_system_services.cpp': [
                'static void test_config_import_valid_json()',
                'static void test_config_transfer_import_selective()'
            ]
        }
    },
    {
        name: 'feature persistence and factory reset durability',
        files: {
            'test/unit/test_feature_config_persistence_contract.py': [
                'def test_registry_factory_config_and_fixture_have_identical_keys',
                'def test_manager_writes_full_snapshot_and_normalizes_sparse_files',
                'def test_network_mdns_compatibility_field_is_synchronized_to_features'
            ],
            'test/unit/test_network_mdns_feature_contract.py': [
                'def test_sparse_snapshot_keeps_missing_mdns_eligible_for_legacy_migration',
                'def test_explicit_features_mdns_overrides_stale_network_mirror',
                'def test_update_rolls_back_when_either_config_file_save_fails',
                'def test_uncompiled_mdns_enable_is_rejected_and_runtime_gate_stays_closed'
            ],
            'test/unit/test_factory_reset_persistence_contract.py': [
                'def test_factory_write_validates_and_removes_older_generations',
                'def test_web_reset_covers_every_known_json_config',
                'def test_web_reset_reports_partial_failure_without_scheduling_reboot',
                'def test_rule_triggered_reset_aborts_reboot_on_any_cleanup_failure'
            ]
        }
    },
    {
        name: 'authentication session reuse and safety',
        files: {
            'test/unit/test_auth_session_contract.py': [
                'def test_invalid_session_copies_username_before_erasing_map_entry',
                'def test_browser_health_checks_do_not_bypass_cached_session_restore',
                'def test_browser_login_retries_follow_server_retry_after'
            ]
        }
    },
    {
        name: 'deployment workflow safety',
        files: {
            'scripts/test-deploy-pio-observability.js': [
                'all deploy.ps1 PIO invocations must pass through the single logging wrapper',
                'web asset generation must never trigger an implicit upload or monitor'
            ],
            'test/unit/test_deploy_config_bundle.py': [
                'def test_staging_excludes_repository_files_not_in_selected_bundle',
                'def test_littlefs_only_mode_is_forwarded_and_audited',
                'def test_sta_credentials_are_injected_at_deployment_time'
            ],
            'scripts/test-test-all-contract.js': [
                'device smoke without BaseUrl must fail',
                'device AP address must fail'
            ]
        }
    },
    {
        name: 'device discovery parsing and deduplication',
        files: {
            'scripts/test-device-discovery.js': [
                'the same device must be merged across passes and MAC casing',
                'the latest pass must replace stale device data',
                'MAC prefix filtering must ignore case and separator style'
            ]
        }
    }
];

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function relative(filePath) {
    return path.relative(ROOT_DIR, filePath).replace(/\\/g, '/');
}

function unique(values) {
    return Array.from(new Set(values));
}

function duplicates(values) {
    const seen = new Set();
    const dupes = new Set();
    values.forEach((value) => {
        if (seen.has(value)) dupes.add(value);
        seen.add(value);
    });
    return Array.from(dupes).sort();
}

function lineNumber(text, index) {
    return text.slice(0, index).split(/\r?\n/).length;
}

function maskCppNonCode(source) {
    const output = source.split('');
    const mask = (start, end) => {
        for (let index = start; index < end; index += 1) {
            if (output[index] !== '\n' && output[index] !== '\r') output[index] = ' ';
        }
    };

    let index = 0;
    while (index < source.length) {
        if (source.startsWith('//', index)) {
            const end = source.indexOf('\n', index + 2);
            const stop = end < 0 ? source.length : end;
            mask(index, stop);
            index = stop;
            continue;
        }
        if (source.startsWith('/*', index)) {
            const end = source.indexOf('*/', index + 2);
            const stop = end < 0 ? source.length : end + 2;
            mask(index, stop);
            index = stop;
            continue;
        }
        if (source[index] === 'R' && source[index + 1] === '"') {
            const openParen = source.indexOf('(', index + 2);
            if (openParen >= 0 && openParen - index <= 18) {
                const delimiter = source.slice(index + 2, openParen);
                if (!/[\s()\\]/.test(delimiter)) {
                    const closeToken = `)${delimiter}"`;
                    const close = source.indexOf(closeToken, openParen + 1);
                    const stop = close < 0 ? source.length : close + closeToken.length;
                    mask(index, stop);
                    index = stop;
                    continue;
                }
            }
        }
        if (source[index] === '"' || source[index] === "'") {
            const quote = source[index];
            const start = index;
            index += 1;
            while (index < source.length) {
                if (source[index] === '\\') {
                    index += 2;
                    continue;
                }
                if (source[index] === quote) {
                    index += 1;
                    break;
                }
                index += 1;
            }
            mask(start, Math.min(index, source.length));
            continue;
        }
        index += 1;
    }
    return output.join('');
}

function findMatchingBrace(code, openingBrace) {
    let depth = 0;
    for (let index = openingBrace; index < code.length; index += 1) {
        if (code[index] === '{') depth += 1;
        if (code[index] === '}') {
            depth -= 1;
            if (depth === 0) return index;
        }
    }
    return -1;
}

function extractTestFunctions(source) {
    const code = maskCppNonCode(source);
    const pattern = /(?:^|\n)\s*(?:static\s+)?(?:inline\s+)?void\s+(test_[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*(?:noexcept\s*)?\{/g;
    const functions = [];
    let match;
    while ((match = pattern.exec(code)) !== null) {
        const openingBrace = code.indexOf('{', match.index);
        const closingBrace = findMatchingBrace(code, openingBrace);
        functions.push({ name: match[1], start: match.index, openingBrace, closingBrace, complete: closingBrace >= 0 });
        if (closingBrace < 0) break;
    }
    return { code, functions };
}

function analyzeTestSource(source) {
    const { code, functions } = extractTestFunctions(source);
    const registrations = [];
    const pattern = /\bRUN_TEST\s*\(\s*(test_[A-Za-z0-9_]+)\s*\)/g;
    let match;
    while ((match = pattern.exec(code)) !== null) {
        const container = functions.find((fn) =>
            fn.complete && match.index > fn.openingBrace && match.index < fn.closingBrace);
        registrations.push({ name: match[1], index: match.index, container: container ? container.name : null });
    }
    return {
        code,
        functions,
        groups: functions.filter((fn) => fn.name.endsWith('_group')),
        tests: functions.filter((fn) => !fn.name.endsWith('_group')),
        registrations
    };
}

function listTestFiles(testDir = TEST_DIR, issues = []) {
    const files = [];
    const walk = (directory) => {
        fs.readdirSync(directory, { withFileTypes: true })
            .sort((left, right) => left.name.localeCompare(right.name))
            .forEach((entry) => {
                const fullPath = path.join(directory, entry.name);
                if (entry.isDirectory()) {
                    if (!EXCLUDED_TEST_DIRS.has(entry.name.toLowerCase())) walk(fullPath);
                    return;
                }
                if (!entry.isFile() || !/^test_.*\.cpp$/i.test(entry.name) || entry.name === 'test_main.cpp') return;
                if (!/^test_.*\.cpp$/.test(entry.name)) {
                    issues.push(`${relative(fullPath)}: native test file names must use lowercase test_ prefix`);
                    return;
                }
                files.push(fullPath);
            });
    };
    walk(testDir);
    return files.sort();
}

function validateTestAnalysis(source, analysis, rel, mainDecls, mainCalls, issues, summary, globalGroups) {
    const groupNames = analysis.groups.map((group) => group.name);
    const testNames = analysis.tests.map((test) => test.name);
    const registrationNames = analysis.registrations.map((registration) => registration.name);

    summary.files += 1;
    summary.groups += analysis.groups.length;
    summary.tests += analysis.registrations.length;

    analysis.functions.filter((fn) => !fn.complete).forEach((fn) => {
        issues.push(`${rel}:${lineNumber(source, fn.start)}: ${fn.name} has an unmatched opening brace`);
    });
    duplicates(groupNames).forEach((name) => issues.push(`${rel}: duplicate group definition ${name}`));
    duplicates(testNames).forEach((name) => issues.push(`${rel}: duplicate local test function ${name}`));
    duplicates(registrationNames).forEach((name) => issues.push(`${rel}: duplicate RUN_TEST(${name}) registration`));

    if (analysis.groups.length === 0) issues.push(`${rel}: missing test_*_group() entry point`);
    if (analysis.registrations.length === 0) issues.push(`${rel}: missing RUN_TEST(...) cases`);

    analysis.groups.forEach((group) => {
        if (!globalGroups.has(group.name)) globalGroups.set(group.name, []);
        globalGroups.get(group.name).push(rel);
        if (!mainDecls.has(group.name)) issues.push(`${rel}: ${group.name} is not declared in test/test_main.cpp`);
        if (!mainCalls.has(group.name)) issues.push(`${rel}: ${group.name} is not called from test/test_main.cpp`);
        if (!analysis.registrations.some((registration) => registration.container === group.name)) {
            issues.push(`${rel}:${lineNumber(source, group.start)}: ${group.name} has no RUN_TEST(...) calls in its body`);
        }
    });

    const localTests = new Set(testNames);
    analysis.registrations.forEach((registration) => {
        if (!registration.container || !registration.container.endsWith('_group')) {
            issues.push(`${rel}:${lineNumber(source, registration.index)}: RUN_TEST(${registration.name}) must be inside a test_*_group() body`);
        }
        if (!localTests.has(registration.name)) {
            issues.push(`${rel}:${lineNumber(source, registration.index)}: RUN_TEST(${registration.name}) has no local definition`);
        }
    });

    analysis.tests.forEach((test) => {
        if (!analysis.registrations.some((registration) => registration.name === test.name)) {
            issues.push(`${rel}:${lineNumber(source, test.start)}: ${test.name} is defined but not registered with RUN_TEST`);
        }
    });
}

function findMatches(text, regex, groupIndex = 1) {
    const values = [];
    let match;
    while ((match = regex.exec(text)) !== null) values.push(match[groupIndex]);
    return values;
}

function extractCiFirmwareEnvironments(ciText) {
    const lines = ciText.split(/\r?\n/);
    const jobStart = lines.findIndex((line) => /^  firmware-build:\s*$/.test(line));
    if (jobStart < 0) return [];
    const jobEndOffset = lines.slice(jobStart + 1).findIndex((line) => /^  [A-Za-z0-9_-]+:\s*$/.test(line));
    const jobEnd = jobEndOffset < 0 ? lines.length : jobStart + 1 + jobEndOffset;
    const environmentLine = lines.findIndex((line, index) =>
        index > jobStart && index < jobEnd && /^\s+environment:\s*$/.test(line));
    if (environmentLine < 0) return [];
    const indent = lines[environmentLine].match(/^\s*/)[0].length;
    const environments = [];
    for (let index = environmentLine + 1; index < jobEnd; index += 1) {
        const line = lines[index];
        if (!line.trim()) continue;
        const lineIndent = line.match(/^\s*/)[0].length;
        if (lineIndent <= indent) break;
        const match = line.match(/^\s*-\s+([A-Za-z0-9_-]+)\s*$/);
        if (match) environments.push(match[1]);
    }
    return environments;
}

function validateSameSet(label, actual, expected, issues) {
    duplicates(actual).forEach((value) => issues.push(`${label}: duplicate ${value}`));
    const actualSet = new Set(actual);
    const expectedSet = new Set(expected);
    expectedSet.forEach((value) => {
        if (!actualSet.has(value)) issues.push(`${label}: missing ${value}`);
    });
    actualSet.forEach((value) => {
        if (!expectedSet.has(value)) issues.push(`${label}: unexpected ${value}`);
    });
}

function validateFirmwareMatrix(issues) {
    const platformioText = readText(PLATFORMIO_FILE);
    const platformioEnvs = findMatches(platformioText, /^\[env:([^\]]+)\]\s*$/gm)
        .filter((name) => name !== 'native');
    const ciText = readText(CI_FILE);
    const ciEnvs = extractCiFirmwareEnvironments(ciText);
    if (ciEnvs.length === 0) issues.push('.github/workflows/ci.yml: firmware-build matrix is missing or empty');
    validateSameSet('.github/workflows/ci.yml firmware-build matrix', ciEnvs, platformioEnvs, issues);
    if (!ciText.includes('pio run -e ${{ matrix.environment }} --target buildfs')) {
        issues.push('.github/workflows/ci.yml: firmware matrix must build LittleFS for every environment');
    }
    if (!ciText.includes('test-build-artifacts.js --env=${{ matrix.environment }} --current-build')) {
        issues.push('.github/workflows/ci.yml: firmware matrix must validate each current build artifact set');
    }
    [
        'node scripts/gzip-www.js --no-upload --no-monitor --stage-dir=.pio/ci-fs-stage',
        'test -f data/www/index.html.gz',
        'test -f data/www/js/chunk-1-core.js.gz',
        'test -f data/www/js/chunk-2-app.js.gz',
        'test -f data/www/js/chunk-3-state.js.gz',
        'test -f data/www/js/modules/dashboard-fullscreen.js.gz',
        'test -f .pio/ci-fs-stage/www/index.html.gz',
        'find data/www -type f',
        'find .pio/ci-fs-stage/www -type f',
        'node scripts/web-smoke-test.js',
        'node scripts/test-gzip-www-contract.js --stage-dir=.pio/ci-fs-stage'
    ].forEach((command) => {
        if (!ciText.includes(command)) {
            issues.push(`.github/workflows/ci.yml: web build is missing current output check: ${command}`);
        }
    });
}

function validateCriticalRegressionContracts(issues, readFile = (rel) => readText(path.join(ROOT_DIR, rel))) {
    CRITICAL_REGRESSION_CONTRACTS.forEach((contract) => {
        Object.entries(contract.files).forEach(([rel, markers]) => {
            let source;
            try {
                source = readFile(rel);
            } catch (error) {
                issues.push(`critical regression ${contract.name}: missing ${rel}`);
                return;
            }
            markers.forEach((marker) => {
                if (!source.includes(marker)) {
                    issues.push(`critical regression ${contract.name}: ${rel} is missing ${marker}`);
                }
            });
        });
    });
}

function validateHostRegressionRegistration(issues, summary) {
    const packageJson = JSON.parse(readText(PACKAGE_FILE));
    const scripts = packageJson.scripts || {};
    const expandNpmScript = (name, seen = new Set()) => {
        if (seen.has(name)) return '';
        seen.add(name);
        const command = scripts[name] || '';
        const nested = [];
        const pattern = /\bnpm\s+run\s+([A-Za-z0-9:_-]+)/g;
        let match;
        while ((match = pattern.exec(command)) !== null) nested.push(expandNpmScript(match[1], seen));
        return `${command} ${nested.join(' ')}`;
    };
    const staticCommands = expandNpmScript('test:static');
    const hostCommands = expandNpmScript('test');
    const regressionCommands = expandNpmScript('test:regression');

    const validators = fs.readdirSync(path.join(ROOT_DIR, 'scripts'))
        .filter((name) => /^validate-.*\.js$/.test(name))
        .sort();
    const wrappedValidators = new Map([
        ['validate-fim-manifest.js', 'scripts/test-fim-manifest-validator.js']
    ]);
    validators.forEach((name) => {
        const wrapper = wrappedValidators.get(name);
        if (!staticCommands.includes(`scripts/${name}`) && !(wrapper && staticCommands.includes(wrapper))) {
            issues.push(`package.json: test:static does not register scripts/${name}`);
        }
    });

    const explicitTestScripts = new Map([
        ['test-build-artifacts.js', 'test:artifacts'],
        ['test-device-health.js', 'test:health']
    ]);
    const nodeTests = fs.readdirSync(path.join(ROOT_DIR, 'scripts'))
        .filter((name) => /^test-.*\.js$/.test(name))
        .sort();
    nodeTests.forEach((name) => {
        const explicitScript = explicitTestScripts.get(name);
        if (explicitScript) {
            if (!(scripts[explicitScript] || '').includes(`scripts/${name}`)) {
                issues.push(`package.json: ${explicitScript} does not register scripts/${name}`);
            }
        } else if (!hostCommands.includes(`scripts/${name}`)) {
            issues.push(`package.json: default host regression does not register scripts/${name}`);
        }
    });

    if (!(scripts.test || '').includes('test:host')) issues.push('package.json: npm test must delegate to test:host');
    ['test:static', 'test:contracts', 'test:unit'].forEach((name) => {
        if (!hostCommands.includes(scripts[name] || '__missing__')) issues.push(`package.json: test:host does not include ${name}`);
    });
    if (!regressionCommands.includes('scripts/run-native-tests.js') ||
        !regressionCommands.includes(scripts['test:host'] || '__missing__')) {
        issues.push('package.json: test:regression must run both host and native regressions');
    }
    if (!(scripts['test:firmware'] || '').includes('scripts/run-firmware-tests.js')) {
        issues.push('package.json: test:firmware must use scripts/run-firmware-tests.js');
    }
    if ((scripts['test:e2e:collect'] || '') !== 'npm --prefix test/browser run test -- --list') {
        issues.push('package.json: test:e2e:collect must parse the isolated browser test project');
    }
    if (!(scripts['test:unit'] || '').includes('scripts/run-unit-tests.js')) {
        issues.push('package.json: test:unit must use scripts/run-unit-tests.js');
    }
    const unitRunner = readText(UNIT_RUNNER_FILE);
    if (!unitRunner.includes("'unittest', 'discover'") || !unitRunner.includes("'-s', 'test/unit'") ||
        !unitRunner.includes("'-p', 'test_*.py'")) {
        issues.push('scripts/run-unit-tests.js: Python tests must discover test/unit/test_*.py');
    }
    const ciText = readText(CI_FILE);
    if (!/^\s*run:\s*npm run test:regression\s*$/m.test(ciText)) {
        issues.push('.github/workflows/ci.yml: regression job must run npm run test:regression');
    }
    if (!/^\s*run:\s*npm run test:e2e:collect\s*$/m.test(ciText) ||
        !/^\s*run:\s*npm ci --prefix test\/browser\s*$/m.test(ciText)) {
        issues.push('.github/workflows/ci.yml: browser test collection must install its isolated dependencies and parse every spec');
    }
    const testAllText = readText(TEST_ALL_FILE);
    if (!testAllText.includes('Invoke-External @("npm", "test")')) {
        issues.push('scripts/test-all.ps1: static gate must reuse npm test');
    }
    if (!/\[string\]\$BaseUrl\s*=\s*""/.test(testAllText) ||
        !testAllText.includes('refuses the device AP address 192.168.4.1')) {
        issues.push('scripts/test-all.ps1: live-device checks must require an explicit non-AP STA URL');
    }
    summary.hostScripts = validators.length + nodeTests.length + 1;
}

function main() {
    const issues = [];
    if (!fs.existsSync(MAIN_FILE)) throw new Error(`Missing ${relative(MAIN_FILE)}`);

    const mainSource = readText(MAIN_FILE);
    const mainCode = maskCppNonCode(mainSource);
    const mainDeclList = findMatches(mainCode, /^\s*extern\s+void\s+(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*;/gm);
    const mainCallList = findMatches(mainCode, /^\s*(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*;/gm);
    const mainDecls = new Set(mainDeclList);
    const mainCalls = new Set(mainCallList);
    const summary = { files: 0, groups: 0, tests: 0, hostScripts: 0 };
    const globalGroups = new Map();
    const files = listTestFiles(TEST_DIR, issues);

    files.forEach((filePath) => {
        const source = readText(filePath);
        validateTestAnalysis(source, analyzeTestSource(source), relative(filePath), mainDecls, mainCalls,
            issues, summary, globalGroups);
    });

    duplicates(mainDeclList).forEach((name) => issues.push(`test/test_main.cpp: duplicate extern declaration ${name}`));
    duplicates(mainCallList).forEach((name) => issues.push(`test/test_main.cpp: duplicate call ${name}`));
    globalGroups.forEach((locations, name) => {
        if (locations.length > 1) issues.push(`${name} is defined in multiple files: ${locations.join(', ')}`);
    });
    mainDecls.forEach((name) => {
        if (!mainCalls.has(name)) issues.push(`test/test_main.cpp: ${name} is declared but not called`);
    });
    mainCalls.forEach((name) => {
        if (!mainDecls.has(name)) issues.push(`test/test_main.cpp: ${name} is called but has no extern declaration`);
        if (!globalGroups.has(name)) issues.push(`test/test_main.cpp: ${name} is called but no test file defines it`);
    });

    validateFirmwareMatrix(issues);
    validateHostRegressionRegistration(issues, summary);
    validateCriticalRegressionContracts(issues);

    if (issues.length > 0) {
        console.error('FastBee test coverage validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exitCode = 1;
        return false;
    }
    console.log(`FastBee test coverage OK: files=${summary.files}, groups=${summary.groups}, cases=${summary.tests}, hostScripts=${summary.hostScripts}, criticalContracts=${CRITICAL_REGRESSION_CONTRACTS.length}`);
    return true;
}

if (require.main === module) main();

module.exports = {
    analyzeTestSource,
    CRITICAL_REGRESSION_CONTRACTS,
    extractCiFirmwareEnvironments,
    listTestFiles,
    maskCppNonCode,
    validateCriticalRegressionContracts,
    validateSameSet,
    validateTestAnalysis
};
