'use strict';

const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');
const {
    resolveRequestGovernorLimits,
    resolveWebBuildContext
} = require('./web-profile');

function listFiles(root, current = root) {
    return fs.readdirSync(current, { withFileTypes: true })
        .sort((left, right) => left.name.localeCompare(right.name))
        .flatMap((entry) => {
            const fullPath = path.join(current, entry.name);
            return entry.isDirectory() ? listFiles(root, fullPath) : [fullPath];
        });
}

function treeSha256(wwwDir) {
    const hash = crypto.createHash('sha256');
    listFiles(wwwDir).forEach((filePath) => {
        hash.update(path.relative(wwwDir, filePath).replace(/\\/g, '/'));
        hash.update('\0');
        hash.update(fs.readFileSync(filePath));
        hash.update('\0');
    });
    return hash.digest('hex');
}

function verifyHtmlAssetReferences(wwwDir, environment) {
    const files = listFiles(wwwDir);
    if (!files.some((file) => ['index.html', 'index.html.gz'].includes(path.relative(wwwDir, file)))) {
        throw new Error(`${environment}: missing index.html or index.html.gz`);
    }
    const references = new Set();
    files.filter((file) => /\.html(?:\.gz)?$/i.test(file)).forEach((file) => {
        const bytes = fs.readFileSync(file);
        const html = (file.endsWith('.gz') ? zlib.gunzipSync(bytes) : bytes).toString('utf8');
        for (const match of html.matchAll(/\b(?:src|data-lazy-src|href)\s*=\s*(['"])(\/assets\/[^'"]+)\1/gi)) {
            const asset = match[2].split(/[?#]/, 1)[0].slice(1);
            references.add(asset);
            if (!files.some((candidate) => {
                const relative = path.relative(wwwDir, candidate).replace(/\\/g, '/');
                return relative === asset || relative === `${asset}.gz`;
            })) {
                throw new Error(`${environment}: ${path.relative(wwwDir, file)} references missing ${asset}`);
            }
        }
    });
    return [...references].sort();
}

function verifyWebReleaseAssets(wwwDir, environment, options = {}) {
    const resolvedDir = path.resolve(wwwDir);
    if (!environment) throw new Error('Web release verification requires an environment');
    if (!fs.existsSync(resolvedDir) || !fs.statSync(resolvedDir).isDirectory()) {
        throw new Error(`Missing Web release directory for ${environment}: ${resolvedDir}`);
    }

    const manifestPath = path.join(resolvedDir, 'web-resource-manifest.json');
    const corePath = path.join(resolvedDir, 'js', 'chunk-1-core.js.gz');
    if (!fs.existsSync(manifestPath)) throw new Error(`${environment}: missing web-resource-manifest.json`);
    if (!fs.existsSync(corePath)) throw new Error(`${environment}: missing js/chunk-1-core.js.gz`);

    const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8').replace(/^\uFEFF/, ''));
    if (manifest.environment !== environment) {
        throw new Error(`${environment}: Web manifest belongs to ${manifest.environment || 'unknown'}`);
    }

    const context = resolveWebBuildContext({
        environment,
        platformioPath: options.platformioPath,
        platformioText: options.platformioText,
        argv: []
    });
    const expected = resolveRequestGovernorLimits(context);
    const expectedPair = [expected.maxConcurrent, expected.maxWeight];
    const manifestPair = [
        manifest.requestGovernor && manifest.requestGovernor.maxConcurrent,
        manifest.requestGovernor && manifest.requestGovernor.maxWeight
    ];
    if (manifestPair[0] !== expectedPair[0] || manifestPair[1] !== expectedPair[1]) {
        throw new Error(
            `${environment}: manifest Request Governor is ${manifestPair.join('/')}, expected ${expectedPair.join('/')}`
        );
    }

    const coreSource = zlib.gunzipSync(fs.readFileSync(corePath)).toString('utf8');
    const match = coreSource.match(
        /WEB_PROFILE_REQUEST_LIMITS\s*=\s*Object\.freeze\(\{\s*maxConcurrent\s*:\s*(\d+)\s*,\s*maxWeight\s*:\s*(\d+)\s*\}\)/
    );
    if (!match) throw new Error(`${environment}: built Request Governor profile is not discoverable`);
    const builtPair = match.slice(1).map(Number);
    if (builtPair[0] !== expectedPair[0] || builtPair[1] !== expectedPair[1]) {
        throw new Error(
            `${environment}: built Request Governor is ${builtPair.join('/')}, expected ${expectedPair.join('/')}`
        );
    }

    const htmlAssetReferences = verifyHtmlAssetReferences(resolvedDir, environment);

    return {
        environment,
        wwwDir: resolvedDir,
        fileCount: listFiles(resolvedDir).length,
        treeSha256: treeSha256(resolvedDir),
        htmlAssetReferences,
        requestGovernor: {
            maxConcurrent: builtPair[0],
            maxWeight: builtPair[1],
            memoryProfile: expected.memoryProfile
        }
    };
}

function readArg(name) {
    const prefix = `--${name}=`;
    const inline = process.argv.find((argument) => argument.startsWith(prefix));
    if (inline) return inline.slice(prefix.length);
    const index = process.argv.indexOf(`--${name}`);
    return index >= 0 ? process.argv[index + 1] : '';
}

if (require.main === module) {
    try {
        const result = verifyWebReleaseAssets(readArg('www-dir'), readArg('env'));
        process.stdout.write(`${JSON.stringify(result)}\n`);
    } catch (error) {
        process.stderr.write(`[WebRelease] ${error.message}\n`);
        process.exitCode = 1;
    }
}

module.exports = { listFiles, treeSha256, verifyWebReleaseAssets };
