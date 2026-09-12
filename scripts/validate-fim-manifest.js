'use strict';

/**
 * WP-95: Validate that online flasher firmware manifest aligns with actual builds.
 * 
 * Checks:
 * 1. Each manifest path file exists
 * 2. File size matches declared Flash capacity (4MB=4194304, 8MB=8388608, etc.)
 * 3. Manifest env set equals build-all-artifacts.ps1 env list
 * 4. No orphaned .bin files not referenced by manifest
 * 
 * The docs path can be supplied through FASTBEE_FIM_DIR or --fim-dir.
 */

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');

const FIM_DEFAULT_DIR = process.env.FASTBEE_FIM_DIR ||
    path.resolve(ROOT_DIR, '..', 'FastBee-doc', 'src', 'device', 'fim');
let FIM_DIR = process.argv.find(arg => arg.startsWith('--fim-dir='))?.split('=')[1] || FIM_DEFAULT_DIR;
const REQUIRE_INPUT = process.argv.includes('--require');

if (!fs.existsSync(FIM_DIR)) {
    const message = `FIM directory not found at ${FIM_DIR}`;
    if (REQUIRE_INPUT) {
        console.error(`✗ ${message}`);
        process.exit(1);
    }
    console.log(`⚠ ${message}. Skipping validation.`);
    console.log('Note: pass --require when a missing input must fail validation.');
    process.exit(0);
}

const MANIFEST_PATH = path.join(FIM_DIR, 'manifest.json');
const BUILD_ALL_SCRIPT = path.join(ROOT_DIR, 'scripts', 'build-all-artifacts.ps1');

const ISSUES = [];
const VALID_ENVS = [
    'esp32-F4R0', 'esp32-F8R0', 'esp32-F8R8',
    'esp32c3-F4R0', 'esp32c6-F4R0', 'esp32c6-F8R0',
    'esp32s3-F8R0', 'esp32s3-F8R8', 'esp32s3-F16R8'
];

const FLASH_CAPABILITIES = {
    '4': 4194304,
    '8': 8388608,
    '16': 16777216
};

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function normalizeManifestPath(manifestPath) {
    // The docs repository historically stores paths as ./fim/<file>, while
    // FIM_DIR already points at that fim directory.
    return manifestPath
        .replace(/^[.][\\/]/, '')
        .replace(/^fim[\\/]/i, '')
        .replace(/^[\\/]+/, '');
}

function inferEnvironment(entry) {
    const source = [
        entry.environment,
        entry.env,
        entry.name,
        entry.path,
        entry.releaseFile,
        entry.files && entry.files['factory-with-fs.bin']
    ].filter(Boolean).join(' ');
    const match = source.match(/(esp32(?:c3|c6|s3)?-F(?:4|8|16)R(?:0|4|8))/i);
    return match ? match[1] : '';
}

function normalizeManifestEntry(entry) {
    const environment = entry.environment || entry.env || inferEnvironment(entry);
    const filePath = entry.path || entry.releaseFile ||
        (entry.files && entry.files['factory-with-fs.bin']) || '';
    return {
        ...entry,
        env: environment,
        name: entry.name || environment,
        path: normalizeManifestPath(filePath),
        flashMb: entry.flashMb ?? entry.flashMB
    };
}

function getManifestEntries(manifestJson) {
    if (Array.isArray(manifestJson.environments)) {
        return manifestJson.environments.map(normalizeManifestEntry);
    }
    if (Array.isArray(manifestJson.files)) {
        return manifestJson.files
            .filter(entry => !entry.role || entry.role === 'merged')
            .map(normalizeManifestEntry);
    }
    return [];
}

function extractBuildEnvs(psScriptText) {
    // Extract Environments array from build-all-artifacts.ps1
    // PowerShell accepts both @(...) and @[...]-style generated fixtures;
    // the repository script uses the normal @(...), so support both forms.
    const match = psScriptText.match(/Environments\s*=\s*\@\s*[\(\[]\s*([\s\S]*?)\s*[\)\]]/);
    if (!match) return [];
    
    const envs = [];
    const lines = match[1].split('\n');
    lines.forEach(line => {
        const envMatch = line.match(/"([^"]+)"/);
        if (envMatch) envs.push(envMatch[1]);
    });
    
    return envs;
}

function validateManifest() {
    try {
        const manifestJson = JSON.parse(readText(MANIFEST_PATH));
        
        console.log('\n[1] Parsing manifest.json...\n');
        
        const manifestEntries = getManifestEntries(manifestJson);
        if (manifestEntries.length === 0) {
            ISSUES.push('manifest.json: missing non-empty "environments" or "files" array');
            return;
        }
        
        const manifestEnvs = new Set();
        const declaredFiles = new Map();
        
        manifestEntries.forEach(env => {
            if (!env.path) {
                ISSUES.push(`Environment ${env.name}: missing "path" field`);
                return;
            }
            
            const normalizedPath = normalizeManifestPath(env.path);
            const fimRoot = path.resolve(FIM_DIR);
            const fullPath = path.resolve(fimRoot, normalizedPath);
            const fimPrefix = fimRoot.endsWith(path.sep) ? fimRoot : `${fimRoot}${path.sep}`;
            if (fullPath !== fimRoot && !fullPath.startsWith(fimPrefix)) {
                ISSUES.push(`Unsafe manifest path outside FIM directory: ${env.path}`);
                return;
            }

            const environment = env.env || inferEnvironment(env) || env.name;
            manifestEnvs.add(environment);
            if (!VALID_ENVS.includes(environment)) {
                ISSUES.push(`Unknown manifest environment: ${environment}`);
            }
            
            if (!fs.existsSync(fullPath)) {
                ISSUES.push(`File not found: ${fullPath}`);
                return;
            }
            
            const fileSize = fs.statSync(fullPath).size;
            declaredFiles.set(normalizedPath, { size: fileSize, env: environment });
            
            // Verify Flash capacity
            const flashMb = env.flashMb;
            if (flashMb && !FLASH_CAPABILITIES[flashMb]) {
                ISSUES.push(`${env.path}: invalid flashMb=${flashMb} (expected 4/8/16)`);
            } else if (flashMb) {
                const expectedSize = FLASH_CAPABILITIES[flashMb];
                if (fileSize !== expectedSize) {
                    ISSUES.push(`${env.path}: size ${fileSize} ≠ declared ${flashMb}MB (${expectedSize})`);
                }
            }
        });
        
        // Check for orphaned files
        console.log('[2] Checking for orphaned .bin files...\n');
        const allBinFiles = fs.readdirSync(FIM_DIR)
            .filter(f => f.endsWith('.bin'));
        
        const referencedPaths = Array.from(declaredFiles.keys());
        const orphaned = allBinFiles.filter(f => !referencedPaths.includes(f));
        
        if (orphaned.length > 0) {
            ISSUES.push(`Orphaned .bin files (not in manifest): ${orphaned.join(', ')}`);
        }
        
        // Compare env sets
        console.log('[3] Comparing environment sets...\n');
        const buildEnvs = extractBuildEnvs(readText(BUILD_ALL_SCRIPT));
        const buildEnvSet = new Set(buildEnvs);
        
        const missingInManifest = Array.from(buildEnvSet).filter(e => !manifestEnvs.has(e));
        const missingInBuild = Array.from(manifestEnvs).filter(e => !buildEnvSet.has(e));
        
        if (missingInManifest.length > 0) {
            ISSUES.push(`Environments in build-all-artifacts but missing in manifest: ${missingInManifest.join(', ')}`);
        }
        
        if (missingInBuild.length > 0) {
            ISSUES.push(`Environments in manifest but not in build-all-artifacts: ${missingInBuild.join(', ')}`);
        }
        
        console.log(`✓ Build environments: ${buildEnvs.length}`);
        console.log(`✓ Manifest environments: ${manifestEnvs.size}`);
        
    } catch (err) {
        ISSUES.push(`Error reading manifest: ${err.message}`);
    }
}

function main() {
    console.log(`\n[WP-95] Validating FIM manifest vs actual builds\n`);
    console.log(`FIM Directory: ${FIM_DIR}\n`);
    
    if (!fs.existsSync(MANIFEST_PATH)) {
        const message = `manifest.json not found at ${MANIFEST_PATH}`;
        if (REQUIRE_INPUT) {
            console.error(`✗ ${message}`);
            process.exit(1);
        }
        console.log(`⚠ ${message}`);
        console.log('Skipping WP-95 validation. Pass --require to make this fatal.\n');
        process.exit(0);
    }
    
    validateManifest();
    
    console.log('\n' + '='.repeat(50));
    if (ISSUES.length > 0) {
        console.error(`WP-95 validation FAILED: ${ISSUES.length} issue(s)\n`);
        ISSUES.forEach(issue => console.error(`- ${issue}`));
        process.exit(1);
    } else {
        console.log('WP-95 PASSED: FIM manifest aligned with actual builds');
        process.exit(0);
    }
}

main();
