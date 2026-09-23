'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

const ROOT = path.resolve(__dirname, '..');
const MANIFEST_PATH = path.join(ROOT, 'docs', 'repository-retention-manifest.json');
const IMAGE_PATTERN = /\.(?:png|jpe?g|gif|webp)$/i;
const SCRIPT_PATTERN = /^scripts\/[^/]+\.(?:js|mjs)$/i;
const REFERENCE_ROOTS = new Set([
    '.github', 'docs', 'scripts', 'test', 'web-src',
    'README.md', 'README.en.md', 'package.json', 'platformio.ini'
]);
const REFERENCE_EXCLUDES = new Set([
    'docs/firmware-web-system-audit-and-improvement-plan.md',
    'docs/repository-retention-manifest.json',
    'scripts/audit-repository-retention.js',
    'scripts/test-repository-retention.js'
]);
const ALLOWED_STATUS = new Set([
    'retain', 'retain-generated', 'needs-owner', 'quarantine-pending'
]);

function normalize(relativePath) {
    return String(relativePath || '').replace(/\\/g, '/').replace(/^\.\//, '').replace(/\/$/, '');
}

function committableFiles(root = ROOT) {
    const result = spawnSync(
        'git', ['ls-files', '-z', '--cached', '--others', '--exclude-standard'],
        { cwd: root, encoding: 'utf8', windowsHide: true }
    );
    if (result.error || result.status !== 0) {
        throw new Error('unable to enumerate Git-committable files');
    }
    return result.stdout.split('\0').filter(Boolean).map(normalize);
}

function sha256(fullPath) {
    return crypto.createHash('sha256').update(fs.readFileSync(fullPath)).digest('hex');
}

function isReferenceSource(relativePath) {
    if (REFERENCE_EXCLUDES.has(relativePath)) return false;
    const first = relativePath.split('/')[0];
    if (!REFERENCE_ROOTS.has(first) && !REFERENCE_ROOTS.has(relativePath)) return false;
    return /(?:^README(?:\.en)?\.md$|\.(?:md|html?|css|js|mjs|ts|tsx|json|ya?ml|ps1|py|ini))$/i.test(relativePath);
}

function findAssetReferences(assetPath, files, root = ROOT) {
    const normalizedAsset = normalize(assetPath);
    const variants = [normalizedAsset, `./${normalizedAsset}`];
    const references = [];
    files.filter(isReferenceSource).forEach((relativePath) => {
        const fullPath = path.join(root, relativePath);
        if (!fs.existsSync(fullPath) || fs.statSync(fullPath).size > 2 * 1024 * 1024) return;
        const text = fs.readFileSync(fullPath, 'utf8');
        if (variants.some((variant) => text.includes(variant))) references.push(relativePath);
    });
    return references.sort();
}

function validateManifest(manifest, files, issues, root = ROOT) {
    if (manifest.schemaVersion !== 1 || !Array.isArray(manifest.entries)) {
        issues.push('retention manifest must use schemaVersion=1 and contain entries[]');
        return new Map();
    }

    const entries = new Map();
    manifest.entries.forEach((entry, index) => {
        const label = `entries[${index}]`;
        const relativePath = normalize(entry.path);
        if (!relativePath || path.isAbsolute(relativePath) || relativePath.split('/').includes('..')) {
            issues.push(`${label}: path must stay inside the repository`);
            return;
        }
        if (entries.has(relativePath)) {
            issues.push(`${label}: duplicate manifest path '${relativePath}'`);
            return;
        }
        if (!entry.owner || !entry.classification || !ALLOWED_STATUS.has(entry.status)) {
            issues.push(`${label}: owner, classification, and a supported status are required`);
        }
        const fullPath = path.join(root, relativePath);
        if (!fs.existsSync(fullPath)) issues.push(`${relativePath}: manifest target is missing`);
        const isTracked = files.includes(relativePath);
        if (typeof entry.tracked === 'boolean' && entry.tracked !== isTracked) {
            issues.push(`${relativePath}: tracked=${entry.tracked} does not match Git inventory (${isTracked})`);
        }
        if (entry.status === 'quarantine-pending' && !entry.reviewAfter) {
            issues.push(`${relativePath}: quarantine-pending entry requires reviewAfter`);
        }
        entries.set(relativePath, entry);
    });
    return entries;
}

function auditRepository(options = {}) {
    const root = path.resolve(options.root || ROOT);
    const manifestPath = path.resolve(options.manifestPath || MANIFEST_PATH);
    const issues = [];
    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
    const files = committableFiles(root);
    const entries = validateManifest(manifest, files, issues, root);

    const images = files.filter((file) => file.startsWith('images/') && IMAGE_PATTERN.test(file));
    const references = {};
    const unreferenced = [];
    images.forEach((image) => {
        references[image] = findAssetReferences(image, files, root);
        if (references[image].length === 0) unreferenced.push(image);
        const entry = entries.get(image);
        if (!entry) {
            issues.push(`${image}: image asset is missing from the retention manifest`);
        } else if (entry.status === 'retain' && references[image].length === 0) {
            issues.push(`${image}: retained documentation asset has no live consumer`);
        }
    });

    const hashes = new Map();
    images.forEach((image) => {
        const digest = sha256(path.join(root, image));
        if (!hashes.has(digest)) hashes.set(digest, []);
        hashes.get(digest).push(image);
    });
    const duplicates = [...hashes.values()].filter((group) => group.length > 1);
    duplicates.forEach((group) => {
        const registered = group.some((image) => {
            const entry = entries.get(image);
            return entry?.classification === 'duplicate' && group.includes(normalize(entry.canonical));
        });
        if (!registered) issues.push(`unregistered duplicate assets: ${group.join(', ')}`);
    });

    const invalidScripts = [];
    files.filter((file) => SCRIPT_PATTERN.test(file)).forEach((script) => {
        const result = spawnSync(process.execPath, ['--check', path.join(root, script)], {
            cwd: root,
            encoding: 'utf8',
            windowsHide: true
        });
        if (result.status === 0) return;
        invalidScripts.push(script);
        const entry = entries.get(script);
        if (!(entry?.status === 'quarantine-pending' && entry?.syntaxExemption === true)) {
            issues.push(`${script}: invalid JavaScript is not covered by a quarantine entry`);
        }
    });

    return { issues, images, references, unreferenced, duplicates, invalidScripts };
}

function main() {
    let report;
    try {
        report = auditRepository();
    } catch (error) {
        console.error(`Repository retention audit failed: ${error.message}`);
        process.exit(1);
    }
    if (report.issues.length > 0) {
        console.error('Repository retention audit failed:');
        report.issues.forEach((issue) => console.error(`- ${issue}`));
        process.exit(1);
    }
    console.log(
        `Repository retention audit OK: ${report.images.length} images, ` +
        `${report.unreferenced.length} unreferenced, ${report.duplicates.length} duplicate group, ` +
        `${report.invalidScripts.length} quarantined invalid script`
    );
}

if (require.main === module) main();

module.exports = {
    auditRepository,
    committableFiles,
    findAssetReferences,
    normalize,
    validateManifest
};
