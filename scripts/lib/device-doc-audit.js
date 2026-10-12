'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const { TextDecoder } = require('util');

const slash = value => value.replace(/\\/g, '/');
const digest = bytes => crypto.createHash('sha256').update(bytes).digest('hex');

function markdownFiles(root) {
    const files = [];
    for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
        const target = path.join(root, entry.name);
        if (entry.isDirectory()) files.push(...markdownFiles(target));
        else if (entry.isFile() && /\.md$/i.test(entry.name)) files.push(target);
    }
    return files.sort();
}

function fencedBlocks(source) {
    const blocks = [];
    const lines = source.split(/\r?\n/);
    let open = null;
    let start = 0;
    for (let index = 0; index < lines.length; index++) {
        const match = lines[index].match(/^ {0,3}(`{3,}|~{3,})([^\s]*)\s*$/);
        if (!open && match) {
            open = { fence: match[1], language: match[2].toLowerCase(), line: index + 1 };
            start = index + 1;
        } else if (open && new RegExp('^ {0,3}' + open.fence[0] + '{' + open.fence.length + ',}\\s*$').test(lines[index])) {
            blocks.push({ ...open, body: lines.slice(start, index).join('\n'), end: index + 1 });
            open = null;
        }
    }
    if (open) blocks.push({ ...open, body: lines.slice(start).join('\n'), end: lines.length, unclosed: true });
    return blocks;
}

function localLinkExists(file, raw, docsRoot) {
    const target = raw.trim().replace(/^<([^>]+)>.*$/, '$1').replace(/\s+["'][^"']*["']$/, '');
    if (!target || /^(?:[a-z][a-z\d+.-]*:|\/\/|#)/i.test(target)) return true;
    let plain;
    try { plain = decodeURIComponent(target.split(/[?#]/)[0]); }
    catch { return false; }
    if (!plain) return true;
    const srcRoot = path.dirname(docsRoot);
    const absolute = plain.startsWith('/') ? path.resolve(srcRoot, '.' + plain) : path.resolve(path.dirname(file), plain);
    const candidates = [absolute, path.join(absolute, 'README.md')];
    if (/\.html$/i.test(absolute)) candidates.push(absolute.replace(/\.html$/i, '.md'));
    if (!path.extname(absolute)) candidates.push(absolute + '.md');
    if (plain.startsWith('/')) {
        candidates.push(path.resolve(srcRoot, '.vuepress/public', '.' + plain));
        const config = path.join(srcRoot, '.vuepress/config.ts');
        const base = fs.existsSync(config) ? fs.readFileSync(config, 'utf8').match(/\bbase:\s*["']([^"']+)["']/)?.[1] : null;
        if (base && plain.startsWith(base)) {
            const withoutBase = plain.slice(base.length);
            const mounted = path.resolve(srcRoot, withoutBase);
            candidates.push(mounted, path.join(mounted, 'README.md'), path.resolve(srcRoot, '.vuepress/public', withoutBase));
            if (/\.html$/i.test(mounted)) candidates.push(mounted.replace(/\.html$/i, '.md'));
        }
    }
    return candidates.some(candidate => fs.existsSync(candidate));
}

function inspectFile(file, docsRoot) {
    const issues = [];
    const bytes = fs.readFileSync(file);
    const relative = slash(path.relative(docsRoot, file));
    let source;
    try { source = new TextDecoder('utf-8', { fatal: true }).decode(bytes); }
    catch { return { path: relative, sha256: digest(bytes), issues: ['invalid UTF-8'], jsonBlocks: 0, localLinks: 0 }; }
    if (source.includes('\uFFFD')) issues.push('replacement character in text');
    const blocks = fencedBlocks(source);
    let jsonBlocks = 0;
    for (const block of blocks) {
        if (block.unclosed) issues.push(`line ${block.line}: unclosed code fence`);
        if (block.language !== 'json') continue;
        jsonBlocks++;
        try { JSON.parse(block.body); }
        catch (error) { issues.push(`line ${block.line}: invalid JSON: ${error.message}`); }
    }
    const lines = source.split(/\r?\n/);
    for (const block of blocks) {
        for (let index = block.line - 1; index < block.end; index++) lines[index] = '';
    }
    let localLinks = 0;
    lines.forEach((line, index) => {
        const patterns = [/!?\[[^\]\n]*\]\((<[^>]+>|[^)\n]+)\)/g, /\b(?:href|src)=["']([^"']+)["']/g, /^\s*\[[^\]]+\]:\s*(<[^>]+>|\S+)/g];
        for (const pattern of patterns) {
            let match;
            while ((match = pattern.exec(line))) {
                if (/^(?:[a-z][a-z\d+.-]*:|\/\/|#)/i.test(match[1])) continue;
                localLinks++;
                if (!localLinkExists(file, match[1], docsRoot)) issues.push(`line ${index + 1}: missing local link ${match[1]}`);
            }
        }
    });
    return { path: relative, sha256: digest(bytes), issues, jsonBlocks, localLinks };
}

function auditDeviceDocs({ docsRoot, firmwareRoot, reviews = [] }) {
    const files = markdownFiles(docsRoot).map(file => inspectFile(file, docsRoot));
    const byPath = new Map(files.map(file => [file.path, file]));
    const issues = [];
    const reviewed = new Set();
    for (const review of reviews) {
        if (!Array.isArray(review.files)) { issues.push('review is missing files array'); continue; }
        for (const entry of review.files) {
            if (!byPath.has(entry.path)) { issues.push(`review references unknown document: ${entry.path}`); continue; }
            if (reviewed.has(entry.path)) issues.push(`duplicate review: ${entry.path}`);
            reviewed.add(entry.path);
            const file = byPath.get(entry.path);
            file.review = entry;
            file.sourceSha256 = {};
            if (!Array.isArray(entry.sources) || !entry.sources.length) issues.push(`review has no source: ${entry.path}`);
            if (!entry.findings || (Array.isArray(entry.findings) && !entry.findings.length)) issues.push(`review has no findings: ${entry.path}`);
            for (const source of entry.sources || []) {
                const target = path.resolve(firmwareRoot, source);
                const relative = path.relative(firmwareRoot, target);
                if (relative.startsWith('..') || path.isAbsolute(relative) || !fs.existsSync(target) || !fs.statSync(target).isFile()) {
                    issues.push(`invalid review source ${entry.path}: ${source}`);
                } else file.sourceSha256[source] = digest(fs.readFileSync(target));
            }
        }
    }
    if (reviews.length) {
        for (const file of files) if (!reviewed.has(file.path)) issues.push(`document not reviewed: ${file.path}`);
    }
    files.forEach(file => file.issues.forEach(issue => issues.push(`${file.path}: ${issue}`)));
    return {
        schemaVersion: 1,
        scope: 'current-workspace-source-and-document-review',
        deviceValidation: 'not-performed',
        reviewedDocuments: reviewed.size,
        documents: files.length,
        jsonBlocks: files.reduce((sum, file) => sum + file.jsonBlocks, 0),
        localLinks: files.reduce((sum, file) => sum + file.localLinks, 0),
        reviewCoverageRequired: reviews.length > 0,
        passed: issues.length === 0,
        issues,
        files
    };
}

module.exports = { auditDeviceDocs, fencedBlocks, inspectFile, localLinkExists };
