'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const TEST_DIR = path.join(ROOT_DIR, 'test');
const MAIN_FILE = path.join(TEST_DIR, 'test_main.cpp');

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function listTestFiles() {
    return fs.readdirSync(TEST_DIR)
        .filter((name) => /^test_.*\.cpp$/.test(name) && name !== 'test_main.cpp')
        .sort()
        .map((name) => path.join(TEST_DIR, name));
}

function findMatches(text, regex, groupIndex = 1) {
    const values = [];
    let match = regex.exec(text);
    while (match) {
        values.push(match[groupIndex]);
        match = regex.exec(text);
    }
    return values;
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

function validateTestFile(filePath, mainDecls, mainCalls, issues, summary, globalGroups) {
    const rel = path.relative(ROOT_DIR, filePath).replace(/\\/g, '/');
    const text = readText(filePath);
    const rawGroupDefinitions = findMatches(
        text,
        /^\s*void\s+(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*\{/gm
    );
    const groupDefinitions = unique(rawGroupDefinitions);
    const rawRunTests = findMatches(text, /\bRUN_TEST\s*\(\s*(test_[A-Za-z0-9_]+)\s*\)/g);
    const runTests = unique(rawRunTests);
    const rawLocalTestFunctions = findMatches(
        text,
        /(?:^|\n)\s*(?:static\s+)?void\s+(test_[A-Za-z0-9_]+)\s*\([^;{}]*\)\s*\{/g
    );
    const localTestFunctions = new Set(rawLocalTestFunctions);

    summary.files += 1;
    summary.groups += groupDefinitions.length;
    summary.tests += rawRunTests.length;

    duplicates(rawGroupDefinitions).forEach((groupName) => {
        issues.push(`${rel}: duplicate group definition ${groupName}`);
    });
    duplicates(rawRunTests).forEach((testName) => {
        issues.push(`${rel}: duplicate RUN_TEST(${testName}) registration`);
    });
    duplicates(rawLocalTestFunctions).forEach((testName) => {
        issues.push(`${rel}: duplicate local test function ${testName}`);
    });

    if (groupDefinitions.length === 0) {
        issues.push(`${rel}: missing test_*_group() entry point`);
    }
    if (runTests.length === 0) {
        issues.push(`${rel}: missing RUN_TEST(...) cases`);
    }

    groupDefinitions.forEach((groupName) => {
        if (!globalGroups.has(groupName)) {
            globalGroups.set(groupName, []);
        }
        globalGroups.get(groupName).push(rel);

        if (!mainDecls.has(groupName)) {
            issues.push(`${rel}: ${groupName} is not declared in test/test_main.cpp`);
        }
        if (!mainCalls.has(groupName)) {
            issues.push(`${rel}: ${groupName} is not called from test/test_main.cpp`);
        }
    });

    const groupBodies = groupDefinitions.map((groupName) => {
        const start = text.indexOf(`void ${groupName}()`);
        return start >= 0 ? { groupName, start } : null;
    }).filter(Boolean);

    runTests.forEach((testName) => {
        if (!localTestFunctions.has(testName)) {
            issues.push(`${rel}: RUN_TEST(${testName}) has no local void ${testName}() definition`);
        }
    });

    localTestFunctions.forEach((testName) => {
        if (testName.endsWith('_group')) return;
        if (!runTests.includes(testName)) {
            const idx = text.search(new RegExp(`(?:^|\\n)\\s*(?:static\\s+)?void\\s+${testName}\\s*\\([^;{}]*\\)\\s*\\{`));
            issues.push(`${rel}:${idx >= 0 ? lineNumber(text, idx) : '?'}: ${testName} is defined but not registered with RUN_TEST`);
        }
    });

    if (groupBodies.length > 0) {
        const lastGroup = groupBodies[groupBodies.length - 1];
        if (text.indexOf('RUN_TEST', lastGroup.start) < 0) {
            issues.push(`${rel}: ${lastGroup.groupName} has no RUN_TEST(...) calls after its definition`);
        }
    }
}

function main() {
    if (!fs.existsSync(MAIN_FILE)) {
        throw new Error(`Missing ${path.relative(ROOT_DIR, MAIN_FILE)}`);
    }

    const mainText = readText(MAIN_FILE);
    const mainDecls = new Set(findMatches(mainText, /^\s*extern\s+void\s+(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*;/gm));
    const mainCalls = new Set(findMatches(mainText, /^\s*(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*;/gm));
    const issues = [];
    const summary = { files: 0, groups: 0, tests: 0 };
    const globalGroups = new Map();

    const files = listTestFiles();
    files.forEach((filePath) => validateTestFile(filePath, mainDecls, mainCalls, issues, summary, globalGroups));

    globalGroups.forEach((locations, groupName) => {
        if (locations.length > 1) {
            issues.push(`${groupName} is defined in multiple files: ${locations.join(', ')}`);
        }
    });

    mainDecls.forEach((groupName) => {
        if (!mainCalls.has(groupName)) {
            issues.push(`test/test_main.cpp: ${groupName} is declared but not called`);
        }
    });

    mainCalls.forEach((groupName) => {
        if (!mainDecls.has(groupName)) {
            issues.push(`test/test_main.cpp: ${groupName} is called but has no extern declaration`);
        }
    });

    const definedGroups = new Set();
    files.forEach((filePath) => {
        findMatches(readText(filePath), /^\s*void\s+(test_[A-Za-z0-9_]+_group)\s*\(\s*\)\s*\{/gm)
            .forEach((groupName) => definedGroups.add(groupName));
    });

    mainCalls.forEach((groupName) => {
        if (!definedGroups.has(groupName)) {
            issues.push(`test/test_main.cpp: ${groupName} is called but no test file defines it`);
        }
    });

    if (issues.length > 0) {
        console.error('FastBee test coverage validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    console.log(`FastBee test coverage OK: files=${summary.files}, groups=${summary.groups}, cases=${summary.tests}`);
}

main();
