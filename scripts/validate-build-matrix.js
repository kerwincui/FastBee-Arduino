'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const PLATFORMIO_PATH = path.join(ROOT_DIR, 'platformio.ini');
const TEST_ALL_PATH = path.join(ROOT_DIR, 'scripts', 'test-all.ps1');
const BUILD_ALL_PATH = path.join(ROOT_DIR, 'scripts', 'build-all-artifacts.ps1');
const COLLECT_ARTIFACTS_PATH = path.join(ROOT_DIR, 'scripts', 'collect-artifacts.ps1');
const FASTBEE_ARTIFACTS_PATH = path.join(ROOT_DIR, 'scripts', 'fastbee-artifacts.py');
const GZIP_WWW_PATH = path.join(ROOT_DIR, 'scripts', 'gzip-www.js');
const README_PATHS = [
    path.join(ROOT_DIR, 'README.md'),
    path.join(ROOT_DIR, 'README.en.md'),
    path.join(ROOT_DIR, 'scripts', 'README.md')
];
const PROFILES = new Set(['lite', 'standard', 'full']);
const TEST_ALL_CHECKS = new Set(['doctor', 'static', 'native', 'build', 'artifacts', 'device-smoke', 'device-soak', 'all']);
const LEGACY_ENV_NAMES = ['esp32-full', 'esp32s3-full'];

const ENV_EXPECTATIONS = {
    'esp32c3-F4R0': {
        profile: 'lite',
        board: 'esp32-c3-devkitm-1',
        partition: 'partitions/fastbee.csv',
        psram: false,
        buildFlags: ['${esp32c3_runtime_flags.build_flags}', '${slim_flags.build_flags}'],
        forbiddenBuildFlags: ['${standard_flags.build_flags}', '${full_flags.build_flags}'],
        libIgnore: ['NimBLE-Arduino']
    },
    'esp32c6-F4R0': {
        profile: 'lite',
        board: 'esp32-c6-devkitc-1',
        partition: 'partitions/fastbee.csv',
        psram: false,
        buildFlags: ['${esp32c6_runtime_flags.build_flags}', '${slim_flags.build_flags}', '-DFASTBEE_ENABLE_DS18B20=0'],
        forbiddenBuildFlags: ['${standard_flags.build_flags}', '${full_flags.build_flags}'],
        libIgnore: ['NimBLE-Arduino', 'OneWire', 'DallasTemperature']
    },
    'esp32-F4R0': {
        profile: 'standard',
        board: 'esp32dev',
        mcu: 'esp32',
        partition: 'partitions/fastbee.csv',
        psram: false,
        buildFlags: ['${esp32_runtime_flags.build_flags}', '${standard_flags.build_flags}', '-DFASTBEE_ENABLE_OTA=0', '-DFASTBEE_ENABLE_OTA_FS=0'],
        forbiddenBuildFlags: ['${slim_flags.build_flags}', '${full_flags.build_flags}'],
        libIgnore: ['NimBLE-Arduino']
    },
    'esp32-F8R4': {
        profile: 'full',
        board: 'esp32dev',
        mcu: 'esp32',
        partition: 'partitions/fastbee-8MB.csv',
        flashSize: '8MB',
        psram: true,
        buildFlags: ['${esp32_runtime_flags.build_flags}', '${full_flags.build_flags}', '-DFASTBEE_ENABLE_IR_REMOTE=1'],
        forbiddenBuildFlags: ['${slim_flags.build_flags}', '${standard_flags.build_flags}']
    },
    'esp32s3-F8R0': {
        profile: 'standard',
        board: 'esp32-s3-devkitc-1',
        partition: 'partitions/fastbee-8MB.csv',
        flashSize: '8MB',
        psram: false,
        buildFlags: ['${esp32s3_runtime_flags.build_flags}', '${standard_flags.build_flags}', '-DFASTBEE_ENABLE_OTA=1', '-DFASTBEE_ENABLE_OTA_FS=1'],
        forbiddenBuildFlags: ['${slim_flags.build_flags}', '${full_flags.build_flags}']
    },
    'esp32s3-F8R4': {
        profile: 'full',
        board: 'esp32-s3-devkitc-1',
        partition: 'partitions/fastbee-8MB.csv',
        flashSize: '8MB',
        psram: true,
        buildFlags: ['${esp32s3_runtime_flags.build_flags}', '${full_flags.build_flags}', '-DFASTBEE_ENABLE_IR_REMOTE=0'],
        forbiddenBuildFlags: ['${slim_flags.build_flags}', '${standard_flags.build_flags}']
    },
    'esp32s3-F16R8': {
        profile: 'full',
        board: 'esp32-s3-devkitc1-n16r8',
        partition: 'partitions/fastbee-16MB.csv',
        flashSize: '16MB',
        psram: true,
        buildFlags: ['${esp32s3_runtime_flags.build_flags}', '${full_flags.build_flags}', '-DFASTBEE_ENABLE_IR_REMOTE=0'],
        forbiddenBuildFlags: ['${slim_flags.build_flags}', '${standard_flags.build_flags}']
    }
};

const FLAG_SECTION_REQUIREMENTS = {
    slim_flags: [
        '-DFASTBEE_USE_PSRAM=0',
        '-DFASTBEE_ENABLE_TCP=0',
        '-DFASTBEE_ENABLE_HTTP=0',
        '-DFASTBEE_ENABLE_COAP=0',
        '-DFASTBEE_ENABLE_MODBUS=0',
        '-DFASTBEE_ENABLE_OTA=0',
        '-DFASTBEE_ENABLE_RULE_SCRIPT=0',
        '-DFASTBEE_ENABLE_COMMAND_SCRIPT=0',
        '-DFASTBEE_ENABLE_USER_ADMIN=0',
        '-DFASTBEE_ENABLE_ROLE_ADMIN=0',
        '-DFASTBEE_ENABLE_FILE_MANAGER=0',
        '-DFASTBEE_ENABLE_BLE=0',
        '-DFASTBEE_ENABLE_I2C_SENSORS=0',
        '-DFASTBEE_ENABLE_RFID=0',
        '-DFASTBEE_ENABLE_IR_REMOTE=0',
        '-DFASTBEE_ENABLE_LORA=0'
    ],
    standard_flags: [
        '-DFASTBEE_USE_PSRAM=0',
        '-DFASTBEE_ENABLE_MODBUS=1',
        '-DFASTBEE_ENABLE_COMMAND_SCRIPT=1',
        '-DFASTBEE_ENABLE_RULE_SCRIPT=0',
        '-DFASTBEE_ENABLE_USER_ADMIN=0',
        '-DFASTBEE_ENABLE_FILE_MANAGER=0'
    ],
    full_flags: [
        '-DFASTBEE_USE_PSRAM=1',
        '-DBOARD_HAS_PSRAM',
        '-DFASTBEE_ENABLE_TCP=1',
        '-DFASTBEE_ENABLE_HTTP=1',
        '-DFASTBEE_ENABLE_COAP=1',
        '-DFASTBEE_ENABLE_MODBUS=1',
        '-DFASTBEE_MODBUS_SLAVE_ENABLE=1',
        '-DFASTBEE_ENABLE_OTA=1',
        '-DFASTBEE_ENABLE_OTA_FS=1',
        '-DFASTBEE_ENABLE_RULE_SCRIPT=1',
        '-DFASTBEE_ENABLE_USER_ADMIN=1',
        '-DFASTBEE_ENABLE_ROLE_ADMIN=1',
        '-DFASTBEE_ENABLE_FILE_MANAGER=1',
        '-DFASTBEE_ENABLE_BLE=1',
        '-DFASTBEE_ENABLE_LORA=1',
        '-DFASTBEE_ENABLE_I18N=1'
    ],
    esp32c3_runtime_flags: [
        '-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=1',
        '-DCONFIG_ASYNC_TCP_QUEUE_SIZE=4',
        '-DARDUINO_LOOP_STACK_SIZE=12288',
        '-DSCRIPT_TASK_STACK=6144',
        '-DSIMPLE_TASK_STACK=4096'
    ],
    esp32c6_runtime_flags: [
        '-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=3',
        '-DCONFIG_ASYNC_TCP_QUEUE_SIZE=8',
        '-DARDUINO_LOOP_STACK_SIZE=12288',
        '-DSCRIPT_TASK_STACK=6144',
        '-DSIMPLE_TASK_STACK=4096'
    ],
    esp32_runtime_flags: [
        '-DCONFIG_ASYNC_TCP_MAX_CONNECTIONS=2',
        '-DCONFIG_ASYNC_TCP_QUEUE_SIZE=4',
        '-DARDUINO_LOOP_STACK_SIZE=16384',
        '-DSCRIPT_TASK_STACK=8192',
        '-DSIMPLE_TASK_STACK=6144'
    ]
};

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function rel(filePath) {
    return path.relative(ROOT_DIR, filePath).replace(/\\/g, '/');
}

function escapeRegex(value) {
    return value.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function findDuplicates(values) {
    const seen = new Set();
    const dupes = new Set();
    values.forEach((value) => {
        if (seen.has(value)) dupes.add(value);
        seen.add(value);
    });
    return Array.from(dupes).sort();
}

function sortedUnique(values) {
    return Array.from(new Set(values)).sort();
}

function setDifference(left, right) {
    const rightSet = new Set(right);
    return sortedUnique(left.filter((item) => !rightSet.has(item)));
}

function validateSameSet(label, actual, expected, issues) {
    const missing = setDifference(expected, actual);
    const extra = setDifference(actual, expected);
    if (missing.length > 0) {
        issues.push(`${label}: missing ${missing.join(', ')}`);
    }
    if (extra.length > 0) {
        issues.push(`${label}: unexpected ${extra.join(', ')}`);
    }
}

function extractIniSection(text, sectionName) {
    const headerRegex = new RegExp(`^\\[${escapeRegex(sectionName)}\\]\\s*$`, 'm');
    const header = headerRegex.exec(text);
    if (!header) return '';
    const start = header.index + header[0].length;
    const rest = text.slice(start);
    const nextSection = /^\[[^\]]+\]\s*$/m.exec(rest);
    return nextSection ? rest.slice(0, nextSection.index) : rest;
}

function extractIniValue(text, sectionName, key) {
    const section = extractIniSection(text, sectionName);
    if (!section) return '';
    const lines = section.split(/\r?\n/);
    for (let i = 0; i < lines.length; i += 1) {
        const match = lines[i].match(new RegExp(`^\\s*${escapeRegex(key)}\\s*=\\s*(.*)$`));
        if (match) return stripIniComment(match[1]);
    }
    return '';
}

function resolveIniValue(text, sectionName, key, stack = []) {
    const value = extractIniValue(text, sectionName, key);
    const ref = value.match(/^\$\{([^}.]+)\.([^}]+)\}$/);
    if (!ref) return value;

    const refKey = `${ref[1]}.${ref[2]}`;
    if (stack.includes(refKey)) return value;
    return resolveIniValue(text, ref[1], ref[2], stack.concat(refKey));
}

function stripIniComment(line) {
    return line.replace(/[;#].*$/, '').trim();
}

function extractIniList(text, sectionName, key) {
    const section = extractIniSection(text, sectionName);
    if (!section) return [];
    const lines = section.split(/\r?\n/);
    const values = [];

    for (let i = 0; i < lines.length; i += 1) {
        const match = lines[i].match(new RegExp(`^\\s*${escapeRegex(key)}\\s*=\\s*(.*)$`));
        if (!match) continue;

        const first = stripIniComment(match[1]);
        if (first) values.push(...first.split(/[,\s]+/).filter(Boolean));

        for (let j = i + 1; j < lines.length; j += 1) {
            const line = lines[j];
            if (!/^\s+/.test(line)) break;
            const cleaned = stripIniComment(line);
            if (cleaned) values.push(...cleaned.split(/[,\s]+/).filter(Boolean));
        }
        break;
    }

    return values;
}

function extractPlatformioEnvNames(text) {
    const envs = [];
    const regex = /^\[env:([^\]\r\n]+)\]\s*$/gm;
    let match = regex.exec(text);
    while (match) {
        envs.push(match[1].trim());
        match = regex.exec(text);
    }
    return envs;
}

function extractPowerShellStringArray(text, variableName) {
    const regex = new RegExp(`\\[string\\[\\]\\]\\s*\\$${escapeRegex(variableName)}\\s*=\\s*@\\(([\\s\\S]*?)\\)`, 'm');
    const match = regex.exec(text);
    if (!match) return [];

    const values = [];
    const itemRegex = /"([^"]+)"/g;
    let item = itemRegex.exec(match[1]);
    while (item) {
        values.push(item[1]);
        item = itemRegex.exec(match[1]);
    }
    return values;
}

function extractPowerShellHashtable(text, variableName) {
    const regex = new RegExp(`\\$${escapeRegex(variableName)}\\s*=\\s*@\\{([\\s\\S]*?)^\\s*\\}`, 'm');
    const match = regex.exec(text);
    if (!match) return new Map();

    const entries = new Map();
    const entryRegex = /^\s*"([^"]+)"\s*=\s*"([^"]*)"/gm;
    let entry = entryRegex.exec(match[1]);
    while (entry) {
        entries.set(entry[1], entry[2]);
        entry = entryRegex.exec(match[1]);
    }
    return entries;
}

function extractPythonStringDict(text, variableName) {
    const regex = new RegExp(`^${escapeRegex(variableName)}\\s*=\\s*\\{([\\s\\S]*?)^\\}`, 'm');
    const match = regex.exec(text);
    if (!match) return new Map();

    const entries = new Map();
    const entryRegex = /^\s*"([^"]+)"\s*:\s*"([^"]*)"/gm;
    let entry = entryRegex.exec(match[1]);
    while (entry) {
        entries.set(entry[1], entry[2]);
        entry = entryRegex.exec(match[1]);
    }
    return entries;
}

function validateNoDuplicates(label, values, issues) {
    findDuplicates(values).forEach((value) => {
        issues.push(`${label}: duplicate ${value}`);
    });
}

function requireTokens(label, actual, expected, issues) {
    expected.forEach((token) => {
        if (!actual.includes(token)) {
            issues.push(`${label}: missing ${token}`);
        }
    });
}

function forbidTokens(label, actual, forbidden, issues) {
    forbidden.forEach((token) => {
        if (actual.includes(token)) {
            issues.push(`${label}: must not include ${token}`);
        }
    });
}

function validateMapCoverage(label, map, expectedKeys, issues) {
    validateSameSet(label, Array.from(map.keys()), expectedKeys, issues);
}

function validateEnvExpectations(platformioText, profileByEnv, fastbeeProfileByEnv, issues) {
    Object.entries(ENV_EXPECTATIONS).forEach(([envName, expectation]) => {
        const sectionName = `env:${envName}`;
        const label = `${rel(PLATFORMIO_PATH)} [${sectionName}]`;
        const section = extractIniSection(platformioText, sectionName);
        if (!section) {
            issues.push(`${label}: missing critical runtime environment`);
            return;
        }

        const board = extractIniValue(platformioText, sectionName, 'board');
        if (board !== expectation.board) {
            issues.push(`${label}: expected board '${expectation.board}', got '${board || '(missing)'}'`);
        }

        if (expectation.mcu) {
            const mcu = resolveIniValue(platformioText, sectionName, 'board_build.mcu');
            if (mcu !== expectation.mcu) {
                issues.push(`${label}: expected board_build.mcu '${expectation.mcu}', got '${mcu || '(missing)'}'`);
            }
        }

        const partition = resolveIniValue(platformioText, sectionName, 'board_build.partitions');
        if (partition !== expectation.partition) {
            issues.push(`${label}: expected board_build.partitions '${expectation.partition}', got '${partition || '(missing)'}'`);
        } else if (!fs.existsSync(path.join(ROOT_DIR, partition))) {
            issues.push(`${label}: partition file does not exist: ${partition}`);
        }

        const flashSize = resolveIniValue(platformioText, sectionName, 'board_build.flash_size');
        if (expectation.flashSize) {
            if (flashSize !== expectation.flashSize) {
                issues.push(`${label}: expected board_build.flash_size '${expectation.flashSize}', got '${flashSize || '(missing)'}'`);
            }
        } else if (flashSize) {
            issues.push(`${label}: 4MB environment should not override board_build.flash_size`);
        }

        const psram = resolveIniValue(platformioText, sectionName, 'board_build.psram');
        if (expectation.psram && psram !== 'enabled') {
            issues.push(`${label}: full profile must set board_build.psram = enabled`);
        }
        if (!expectation.psram && psram) {
            issues.push(`${label}: non-full profile must not set board_build.psram`);
        }

        const buildFlags = extractIniList(platformioText, sectionName, 'build_flags');
        requireTokens(`${label} build_flags`, buildFlags, expectation.buildFlags, issues);
        forbidTokens(`${label} build_flags`, buildFlags, expectation.forbiddenBuildFlags, issues);

        const resolvedBuildFlags = resolveIniBuildFlags(platformioText, sectionName);
        if (expectation.psram) {
            requireTokens(`${label} resolved build_flags`, resolvedBuildFlags, ['-DFASTBEE_USE_PSRAM=1', '-DBOARD_HAS_PSRAM'], issues);
        } else {
            forbidTokens(`${label} resolved build_flags`, resolvedBuildFlags, ['-DBOARD_HAS_PSRAM'], issues);
        }

        const libIgnore = extractIniList(platformioText, sectionName, 'lib_ignore');
        requireTokens(`${label} lib_ignore`, libIgnore, expectation.libIgnore || [], issues);

        const buildAllProfile = profileByEnv.get(envName);
        if (buildAllProfile !== expectation.profile) {
            issues.push(`${rel(BUILD_ALL_PATH)} ProfileByEnv[${envName}]: expected '${expectation.profile}', got '${buildAllProfile || '(missing)'}'`);
        }

        const hookProfile = fastbeeProfileByEnv.get(envName);
        if (hookProfile !== expectation.profile) {
            issues.push(`${rel(FASTBEE_ARTIFACTS_PATH)} ENV_TO_PROFILE[${envName}]: expected '${expectation.profile}', got '${hookProfile || '(missing)'}'`);
        }
    });
}

function validateFlagSectionRequirements(platformioText, issues) {
    Object.entries(FLAG_SECTION_REQUIREMENTS).forEach(([sectionName, requiredFlags]) => {
        const flags = extractIniList(platformioText, sectionName, 'build_flags');
        if (flags.length === 0) {
            issues.push(`${rel(PLATFORMIO_PATH)} [${sectionName}] build_flags: missing or empty`);
            return;
        }
        requireTokens(`${rel(PLATFORMIO_PATH)} [${sectionName}] build_flags`, flags, requiredFlags, issues);
    });
}

function resolveIniBuildFlags(platformioText, sectionName, stack = []) {
    if (stack.includes(sectionName)) {
        return [];
    }

    const rawFlags = extractIniList(platformioText, sectionName, 'build_flags');
    const resolved = [];
    rawFlags.forEach((flag) => {
        const ref = flag.match(/^\$\{([^}.]+)\.build_flags\}$/);
        if (ref) {
            resolved.push(...resolveIniBuildFlags(platformioText, ref[1], stack.concat(sectionName)));
            return;
        }
        resolved.push(flag);
    });
    return resolved;
}

function validateNoConflictingMacroDefines(platformioText, allPlatformioEnvs, issues) {
    allPlatformioEnvs.forEach((envName) => {
        if (envName === 'native') return;
        const sectionName = `env:${envName}`;
        const flags = resolveIniBuildFlags(platformioText, sectionName);
        const defines = new Map();

        flags.forEach((flag) => {
            const match = flag.match(/^-D([^=\s]+)(?:=(.*))?$/);
            if (!match) return;
            const name = match[1];
            const value = match[2] === undefined ? '' : match[2];
            if (!defines.has(name)) {
                defines.set(name, []);
            }
            defines.get(name).push(value);
        });

        defines.forEach((values, name) => {
            const uniqueValues = sortedUnique(values);
            if (uniqueValues.length > 1) {
                issues.push(`${rel(PLATFORMIO_PATH)} [${sectionName}] build_flags: conflicting macro ${name} (${values.join(', ')})`);
            }
        });
    });
}

function validateOutputNames(outputNameByEnv, collectEnvMap, firmwareEnvs, issues) {
    const names = [];
    firmwareEnvs.forEach((envName) => {
        const outputName = outputNameByEnv.get(envName);
        if (!outputName) return;
        names.push(outputName);
        if (!/^fastbee-[a-z0-9]+-F\d+R\d+\.bin$/.test(outputName)) {
            issues.push(`build-all-artifacts.ps1 OutputNameByEnv[${envName}]: invalid release file name '${outputName}'`);
        }
        const expectedSuffix = `${envName}.bin`;
        if (!outputName.endsWith(expectedSuffix)) {
            issues.push(`build-all-artifacts.ps1 OutputNameByEnv[${envName}]: expected file name to end with '${expectedSuffix}'`);
        }

        const collectName = collectEnvMap.get(envName);
        if (collectName && collectName !== outputName) {
            issues.push(`collect-artifacts.ps1 EnvMap[${envName}]: '${collectName}' does not match build-all-artifacts.ps1 '${outputName}'`);
        }
    });

    findDuplicates(names).forEach((value) => {
        issues.push(`build-all-artifacts.ps1 OutputNameByEnv: duplicate release file name '${value}'`);
    });
}

function lineNumberAt(text, index) {
    return text.slice(0, index).split(/\r?\n/).length;
}

function extractReadmeScriptReferences(text) {
    const refs = [];
    const regex = /scripts[\\/][A-Za-z0-9_.-]+(?:[\\/][A-Za-z0-9_.-]+)*\.(?:ps1|js|py|json)/g;
    let match = regex.exec(text);
    while (match) {
        refs.push({ value: match[0], index: match.index });
        match = regex.exec(text);
    }
    return refs;
}

function extractReadmeOptionValues(line, optionName) {
    const pattern = escapeRegex(optionName) + '\\s+([^|`\\r\\n]+)';
    const match = new RegExp(pattern).exec(line);
    if (!match) return [];

    const values = [];
    const tokens = match[1].trim().split(/\s+/).filter(Boolean);
    for (let i = 0; i < tokens.length; i += 1) {
        const token = tokens[i].replace(/[`"'.,]+$/, '');
        if (!token || token.startsWith('-')) break;
        values.push(...token.split(',').map((item) => item.trim()).filter(Boolean));
    }
    return values;
}

function validateReadmeScriptExamples(firmwareEnvs, allPlatformioEnvs, outputNameByEnv, issues) {
    const firmwareEnvSet = new Set(firmwareEnvs);
    const platformioEnvSet = new Set(allPlatformioEnvs);
    const releaseFiles = new Set(Array.from(outputNameByEnv.values()).filter(Boolean));

    README_PATHS.forEach((readmePath) => {
        if (!fs.existsSync(readmePath)) {
            issues.push(`${rel(readmePath)}: missing README file`);
            return;
        }

        const text = readText(readmePath);
        const label = rel(readmePath);

        LEGACY_ENV_NAMES.forEach((envName) => {
            const index = text.indexOf(envName);
            if (index >= 0) {
                issues.push(`${label}:${lineNumberAt(text, index)}: legacy environment '${envName}' is not accepted by deploy scripts`);
            }
        });

        extractReadmeScriptReferences(text).forEach((refInfo) => {
            const normalizedRef = refInfo.value.replace(/\\/g, path.sep).replace(/\//g, path.sep);
            const fullPath = path.join(ROOT_DIR, normalizedRef);
            if (!fs.existsSync(fullPath)) {
                issues.push(`${label}:${lineNumberAt(text, refInfo.index)}: referenced script does not exist: ${refInfo.value}`);
            }
        });

        const envRegex = /(?:^|[\s`])-(Env)\s+([A-Za-z0-9-]+)/g;
        let envMatch = envRegex.exec(text);
        while (envMatch) {
            const envName = envMatch[2];
            if (!firmwareEnvSet.has(envName)) {
                issues.push(`${label}:${lineNumberAt(text, envMatch.index)}: -Env references unknown firmware environment '${envName}'`);
            }
            envMatch = envRegex.exec(text);
        }

        const pioEnvRegex = /(?:^|[\s`])-e\s+([A-Za-z0-9-]+)/g;
        let pioEnvMatch = pioEnvRegex.exec(text);
        while (pioEnvMatch) {
            const envName = pioEnvMatch[1];
            if (!platformioEnvSet.has(envName)) {
                issues.push(`${label}:${lineNumberAt(text, pioEnvMatch.index)}: -e references unknown PlatformIO environment '${envName}'`);
            }
            pioEnvMatch = pioEnvRegex.exec(text);
        }

        const releaseRegex = /fastbee-[a-z0-9]+-F\d+R\d+\.bin/g;
        let releaseMatch = releaseRegex.exec(text);
        while (releaseMatch) {
            const fileName = releaseMatch[0];
            if (!releaseFiles.has(fileName)) {
                issues.push(`${label}:${lineNumberAt(text, releaseMatch.index)}: release image '${fileName}' is not produced by build-all-artifacts.ps1`);
            }
            releaseMatch = releaseRegex.exec(text);
        }

        text.split(/\r?\n/).forEach((line, index) => {
            if (!line.includes('-Checks')) return;
            const usesFile = /powershell\b.*-File\b.*scripts[\\/]test-all\.ps1/i.test(line);
            const checkValues = extractReadmeOptionValues(line, '-Checks');
            if (usesFile && checkValues.length > 1) {
                issues.push(`${label}:${index + 1}: use powershell -Command for multiple -Checks values`);
            }
            checkValues.forEach((checkName) => {
                if (!TEST_ALL_CHECKS.has(checkName)) {
                    issues.push(`${label}:${index + 1}: -Checks references unknown test step '${checkName}'`);
                }
            });
        });
    });
}

function validateGzipWwwEnvironmentFallback(gzipWwwText, firmwareEnvs, defaultEnvs, issues) {
    const defaultEnv = defaultEnvs[0] || 'esp32-F4R0';
    const fallbackRegex = /return\s+(?:activeEnv\s*\|\|\s*)?['"]([^'"]+)['"]\s*;/g;
    let match = fallbackRegex.exec(gzipWwwText);
    while (match) {
        const envName = match[1];
        if (envName === 'esp32' || envName === 'esp32s3' || LEGACY_ENV_NAMES.includes(envName)) {
            issues.push(`${rel(GZIP_WWW_PATH)}:${lineNumberAt(gzipWwwText, match.index)}: legacy fallback environment '${envName}'`);
        } else if (envName.includes('-') && !firmwareEnvs.includes(envName)) {
            issues.push(`${rel(GZIP_WWW_PATH)}:${lineNumberAt(gzipWwwText, match.index)}: fallback environment '${envName}' is not a firmware environment`);
        } else if (envName.includes('-') && envName !== defaultEnv) {
            issues.push(`${rel(GZIP_WWW_PATH)}:${lineNumberAt(gzipWwwText, match.index)}: fallback environment '${envName}' should match default_envs '${defaultEnv}'`);
        }
        match = fallbackRegex.exec(gzipWwwText);
    }
}

function validatePartitionTables(platformioText, issues) {
    const PARTITION_DIR = path.join(ROOT_DIR, 'partitions');
    const partitionEnvs = Object.entries(ENV_EXPECTATIONS);
    const checkedFiles = new Set();
    const otaEnvs = new Set();
    const nonOtaEnvs = new Set();

    partitionEnvs.forEach(([envName, exp]) => {
        if (exp.profile === 'full') {
            otaEnvs.add(envName);
        } else {
            nonOtaEnvs.add(envName);
        }
    });

    partitionEnvs.forEach(([envName, exp]) => {
        const partitionFile = exp.partition;
        if (checkedFiles.has(partitionFile)) return;
        checkedFiles.add(partitionFile);

        const fullPath = path.join(ROOT_DIR, partitionFile);
        if (!fs.existsSync(fullPath)) return;

        const text = fs.readFileSync(fullPath, 'utf8');
        const label = rel(fullPath);
        const rows = [];

        text.split(/\r?\n/).forEach((line) => {
            const trimmed = line.trim();
            if (!trimmed || trimmed.startsWith('#')) return;
            const fields = trimmed.split(',').map((f) => f.trim());
            if (fields.length < 5) return;
            rows.push({ name: fields[0], type: fields[1], subtype: fields[2], offset: fields[3], size: fields[4] });
        });

        if (rows.length === 0) {
            issues.push(`${label}: no valid partition entries found`);
            return;
        }

        const hasOta0 = rows.some((r) => r.subtype === 'ota_0');
        const hasOta1 = rows.some((r) => r.subtype === 'ota_1');
        const hasSpiffs = rows.some((r) => r.subtype === 'spiffs');
        const hasNvs = rows.some((r) => r.subtype === 'nvs');
        const hasOtadata = rows.some((r) => r.subtype === 'ota' && r.type === 'data');

        if (!hasNvs) {
            issues.push(`${label}: missing NVS partition`);
        }
        if (!hasOtadata) {
            issues.push(`${label}: missing OTA data partition`);
        }
        if (!hasSpiffs) {
            issues.push(`${label}: missing SPIFFS/LittleFS partition`);
        }

        // 4MB partition should have single app slot (ota_0 only)
        // 8MB/16MB partition should have dual app slots (ota_0 + ota_1)
        const isSmallPartition = partitionFile.includes('fastbee.csv') && !partitionFile.includes('8MB') && !partitionFile.includes('16MB');

        if (isSmallPartition) {
            if (hasOta1) {
                issues.push(`${label}: 4MB partition should not have dual OTA slots`);
            }
            if (!hasOta0) {
                issues.push(`${label}: missing app partition (ota_0)`);
            }
        } else {
            if (!hasOta0) {
                issues.push(`${label}: missing app0 partition (ota_0)`);
            }
            if (!hasOta1) {
                issues.push(`${label}: missing app1 partition (ota_1) for OTA support`);
            }
        }

        // Check for overlapping partitions
        const parsedOffsets = [];
        rows.forEach((r) => {
            const start = parseInt(r.offset, 16);
            const sizeVal = parseInt(r.size, 16);
            if (isNaN(start) || isNaN(sizeVal)) return;
            parsedOffsets.push({ name: r.name, start, end: start + sizeVal });
        });
        parsedOffsets.sort((a, b) => a.start - b.start);
        for (let i = 1; i < parsedOffsets.length; i++) {
            if (parsedOffsets[i].start < parsedOffsets[i - 1].end) {
                issues.push(`${label}: partition overlap between '${parsedOffsets[i - 1].name}' and '${parsedOffsets[i].name}'`);
            }
        }
    });

    // Verify OTA environments use dual-slot partitions, non-OTA use single-slot
    otaEnvs.forEach((envName) => {
        const partition = ENV_EXPECTATIONS[envName].partition;
        if (partition.includes('fastbee.csv') && !partition.includes('8MB') && !partition.includes('16MB')) {
            issues.push(`${rel(PLATFORMIO_PATH)} [env:${envName}]: full profile should use dual-slot partition table`);
        }
    });
    nonOtaEnvs.forEach((envName) => {
        const partition = ENV_EXPECTATIONS[envName].partition;
        if (partition.includes('8MB') || partition.includes('16MB')) {
            // Non-full environments using 8MB/16MB is OK (e.g., esp32s3-F8R0 is standard with 8MB)
        }
    });
}

function main() {
    const platformioText = readText(PLATFORMIO_PATH);
    const testAllText = readText(TEST_ALL_PATH);
    const buildAllText = readText(BUILD_ALL_PATH);
    const collectArtifactsText = readText(COLLECT_ARTIFACTS_PATH);
    const fastbeeArtifactsText = readText(FASTBEE_ARTIFACTS_PATH);
    const gzipWwwText = readText(GZIP_WWW_PATH);
    const issues = [];

    const allPlatformioEnvs = extractPlatformioEnvNames(platformioText);
    const firmwareEnvs = allPlatformioEnvs.filter((envName) => envName !== 'native');
    const defaultEnvs = extractIniList(platformioText, 'platformio', 'default_envs');
    const testAllEnvs = extractPowerShellStringArray(testAllText, 'Environments');
    const buildAllEnvs = extractPowerShellStringArray(buildAllText, 'Environments');
    const profileByEnv = extractPowerShellHashtable(buildAllText, 'ProfileByEnv');
    const outputNameByEnv = extractPowerShellHashtable(buildAllText, 'OutputNameByEnv');
    const hardwareByEnv = extractPowerShellHashtable(buildAllText, 'HardwareByEnv');
    const collectEnvMap = extractPowerShellHashtable(collectArtifactsText, 'EnvMap');
    const fastbeeProfileByEnv = extractPythonStringDict(fastbeeArtifactsText, 'ENV_TO_PROFILE');

    if (allPlatformioEnvs.length === 0) {
        issues.push(`${rel(PLATFORMIO_PATH)}: no [env:*] sections found`);
    }
    if (!allPlatformioEnvs.includes('native')) {
        issues.push(`${rel(PLATFORMIO_PATH)}: missing [env:native] for host tests`);
    }
    validateNoDuplicates(`${rel(PLATFORMIO_PATH)} env sections`, allPlatformioEnvs, issues);

    if (defaultEnvs.length === 0) {
        issues.push(`${rel(PLATFORMIO_PATH)}: [platformio] default_envs is empty`);
    }
    defaultEnvs.forEach((envName) => {
        if (!firmwareEnvs.includes(envName)) {
            issues.push(`${rel(PLATFORMIO_PATH)}: default_envs references non-firmware environment '${envName}'`);
        }
    });

    validateNoDuplicates(`${rel(TEST_ALL_PATH)} default Environments`, testAllEnvs, issues);
    validateNoDuplicates(`${rel(BUILD_ALL_PATH)} default Environments`, buildAllEnvs, issues);
    validateSameSet(`${rel(TEST_ALL_PATH)} default Environments`, testAllEnvs, firmwareEnvs, issues);
    validateSameSet(`${rel(BUILD_ALL_PATH)} default Environments`, buildAllEnvs, firmwareEnvs, issues);

    validateMapCoverage(`${rel(BUILD_ALL_PATH)} ProfileByEnv`, profileByEnv, firmwareEnvs, issues);
    validateMapCoverage(`${rel(BUILD_ALL_PATH)} OutputNameByEnv`, outputNameByEnv, firmwareEnvs, issues);
    validateMapCoverage(`${rel(BUILD_ALL_PATH)} HardwareByEnv`, hardwareByEnv, firmwareEnvs, issues);
    validateMapCoverage(`${rel(COLLECT_ARTIFACTS_PATH)} EnvMap`, collectEnvMap, firmwareEnvs, issues);
    validateMapCoverage(`${rel(FASTBEE_ARTIFACTS_PATH)} ENV_TO_PROFILE`, fastbeeProfileByEnv, firmwareEnvs, issues);

    profileByEnv.forEach((profile, envName) => {
        if (!PROFILES.has(profile)) {
            issues.push(`${rel(BUILD_ALL_PATH)} ProfileByEnv[${envName}]: unsupported profile '${profile}'`);
        }
        const hookProfile = fastbeeProfileByEnv.get(envName);
        if (hookProfile && hookProfile !== profile) {
            issues.push(`${rel(FASTBEE_ARTIFACTS_PATH)} ENV_TO_PROFILE[${envName}]: '${hookProfile}' does not match build-all-artifacts.ps1 '${profile}'`);
        }
    });
    hardwareByEnv.forEach((hardware, envName) => {
        if (!hardware.trim()) {
            issues.push(`${rel(BUILD_ALL_PATH)} HardwareByEnv[${envName}]: hardware description is empty`);
        }
    });

    validateOutputNames(outputNameByEnv, collectEnvMap, firmwareEnvs, issues);
    validateEnvExpectations(platformioText, profileByEnv, fastbeeProfileByEnv, issues);
    validateFlagSectionRequirements(platformioText, issues);
    validateNoConflictingMacroDefines(platformioText, allPlatformioEnvs, issues);
    validateReadmeScriptExamples(firmwareEnvs, allPlatformioEnvs, outputNameByEnv, issues);
    validateGzipWwwEnvironmentFallback(gzipWwwText, firmwareEnvs, defaultEnvs, issues);
    validatePartitionTables(platformioText, issues);

    if (issues.length > 0) {
        console.error('FastBee build matrix validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    console.log(`FastBee build matrix OK: firmwareEnvs=${firmwareEnvs.length}, default=${defaultEnvs.join(',')}`);
}

main();
