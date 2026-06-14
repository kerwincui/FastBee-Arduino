'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const TARGETS = [
    path.join(ROOT_DIR, 'README.md'),
    path.join(ROOT_DIR, 'README.en.md'),
    path.join(ROOT_DIR, 'docs'),
    path.join(ROOT_DIR, 'scripts'),
    path.join(ROOT_DIR, 'test')
];
const LEGACY_ENV_NAMES = [
    'esp32s3-full',
    'esp32-full'
];
const LEGACY_ENV_COMMAND_PATTERNS = [
    { regex: /(^|[\s"`'])-Env\s+esp32(?!-)(?=$|[\s"`'])/g, message: 'legacy deploy environment -Env esp32; use esp32-F4R0' },
    { regex: /(^|[\s"`'])-Env\s+esp32s3(?!-)(?=$|[\s"`'])/g, message: 'legacy deploy environment -Env esp32s3; use esp32s3-F8R0' },
    { regex: /(^|[\s"`'])-Env\s+esp32c3(?!-)(?=$|[\s"`'])/g, message: 'legacy deploy environment -Env esp32c3; use esp32c3-F4R0' },
    { regex: /(^|[\s"`'])-Env\s+esp32c6(?!-)(?=$|[\s"`'])/g, message: 'legacy deploy environment -Env esp32c6; use esp32c6-F4R0' },
    { regex: /\bpio\s+run\s+-e\s+esp32(?!-)(?=$|[\s"`'])/g, message: 'legacy PlatformIO environment esp32; use esp32-F4R0' },
    { regex: /\bpio\s+run\s+-e\s+esp32s3(?!-)(?=$|[\s"`'])/g, message: 'legacy PlatformIO environment esp32s3; use esp32s3-F8R0' },
    { regex: /\bpio\s+run\s+-e\s+esp32c3(?!-)(?=$|[\s"`'])/g, message: 'legacy PlatformIO environment esp32c3; use esp32c3-F4R0' },
    { regex: /\bpio\s+run\s+-e\s+esp32c6(?!-)(?=$|[\s"`'])/g, message: 'legacy PlatformIO environment esp32c6; use esp32c6-F4R0' }
];

function walkMarkdown(target, files) {
    if (!fs.existsSync(target)) return;
    const stat = fs.statSync(target);
    if (stat.isDirectory()) {
        fs.readdirSync(target).forEach((entry) => walkMarkdown(path.join(target, entry), files));
        return;
    }
    if (stat.isFile() && target.toLowerCase().endsWith('.md')) {
        files.push(target);
    }
}

function rel(filePath) {
    return path.relative(ROOT_DIR, filePath).replace(/\\/g, '/');
}

function lineNumberAt(text, index) {
    return text.slice(0, index).split(/\r?\n/).length;
}

function stripCodeFences(text) {
    return text.replace(/```[\s\S]*?```/g, (block) => '\n'.repeat(block.split(/\r?\n/).length - 1));
}

function parseMarkdownTarget(rawTarget) {
    let value = String(rawTarget || '').trim();
    if (!value) return '';

    if (value.startsWith('<')) {
        const end = value.indexOf('>');
        if (end > 0) return value.slice(1, end).trim();
    }

    const match = value.match(/^([^ \t\r\n]+)(?:\s+["'][^"']*["'])?$/);
    return match ? match[1].trim() : value.split(/\s+/)[0].trim();
}

function isExternalOrVirtual(target) {
    const lower = target.toLowerCase();
    return lower.startsWith('http://') ||
        lower.startsWith('https://') ||
        lower.startsWith('mailto:') ||
        lower.startsWith('tel:') ||
        lower.startsWith('data:') ||
        lower.startsWith('javascript:') ||
        lower.startsWith('#') ||
        lower.startsWith('/api/') ||
        lower.startsWith('{{') ||
        lower.includes('<device-ip>');
}

function normalizeLocalTarget(target) {
    const hashIndex = target.indexOf('#');
    const queryIndex = target.indexOf('?');
    let end = target.length;
    if (hashIndex >= 0) end = Math.min(end, hashIndex);
    if (queryIndex >= 0) end = Math.min(end, queryIndex);
    return target.slice(0, end).replace(/\\/g, '/');
}

function decodeTarget(target) {
    try {
        return decodeURIComponent(target);
    } catch (_) {
        return target;
    }
}

function checkTarget(sourceFile, text, index, target, issues) {
    const parsed = parseMarkdownTarget(target);
    if (!parsed || isExternalOrVirtual(parsed)) return;

    const localTarget = decodeTarget(normalizeLocalTarget(parsed));
    if (!localTarget) return;
    if (path.isAbsolute(localTarget)) return;

    const resolved = path.resolve(path.dirname(sourceFile), localTarget);
    if (fs.existsSync(resolved)) return;

    issues.push(`${rel(sourceFile)}:${lineNumberAt(text, index)}: missing local link '${parsed}'`);
}

function validateFile(filePath, issues) {
    const original = fs.readFileSync(filePath, 'utf8');
    const text = stripCodeFences(original);

    const markdownLink = /!?\[[^\]\r\n]*\]\(([^)\r\n]+)\)/g;
    let match = markdownLink.exec(text);
    while (match) {
        checkTarget(filePath, original, match.index, match[1], issues);
        match = markdownLink.exec(text);
    }

    const htmlLink = /\b(?:src|href)=["']([^"']+)["']/gi;
    match = htmlLink.exec(text);
    while (match) {
        checkTarget(filePath, original, match.index, match[1], issues);
        match = htmlLink.exec(text);
    }

    LEGACY_ENV_NAMES.forEach((name) => {
        const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
        const regex = new RegExp(`\\b${escaped}\\b`, 'g');
        let item = regex.exec(original);
        while (item) {
            issues.push(`${rel(filePath)}:${lineNumberAt(original, item.index)}: legacy environment '${name}'`);
            item = regex.exec(original);
        }
    });

    LEGACY_ENV_COMMAND_PATTERNS.forEach(({ regex, message }) => {
        let item = regex.exec(original);
        while (item) {
            issues.push(`${rel(filePath)}:${lineNumberAt(original, item.index)}: ${message}`);
            item = regex.exec(original);
        }
    });
}

function main() {
    const files = [];
    TARGETS.forEach((target) => walkMarkdown(target, files));
    const issues = [];

    files.sort().forEach((filePath) => validateFile(filePath, issues));

    if (issues.length > 0) {
        console.error('FastBee documentation link validation failed:');
        issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }

    console.log(`FastBee documentation links OK: files=${files.length}`);
}

main();
