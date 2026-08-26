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
 * Default reads from external docs repo (D:\project\gitlab\FastBee-doc\src\device\fim)
 * Path can be overridden via --fim-dir argument.
 */

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');

// Default to external docs repo; use --fim-dir arg to override for testing
const FIM_DEFAULT_DIR = 'D:\\project\\gitlab\\FastBee-doc\\src\\device\\fim';
let FIM_DIR = process.argv.find(arg => arg.startsWith('--fim-dir='))?.split('=')[1] || FIM_DEFAULT_DIR;

if (!fs.existsSync(FIM_DIR)) {
    console.log(`⚠ FIM directory not found at ${FIM_DIR}. Skipping validation.`);
    console.log('Note: This is expected if the docs repo is not available.');
    process.exit(0);
}

const MANIFEST_PATH = path.join(FIM_DIR, 'manifest.json');
const BUILD_ALL_SCRIPT = path.join(ROOT_DIR, 'scripts', 'build-all-artifacts.ps1');

const ISSUES = [];
const VALID_ENVS = ['esp32-F4R0', 'esp32c3-F4R0', 'esp32c6-F4R0', 'esp32s3-F8R4', 'esp32s3-F16R8'];

const FLASH_CAPABILITIES = {
    '4': 4194304,
    '8': 8388608,
    '16': 16777216
};

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function extractBuildEnvs(psScriptText) {
    // Extract Environments array from build-all-artifacts.ps1
    const match = psScriptText.match(/Environments\s*=\s*\@\s*\[\s*([^]]+)\]/s);
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
        
        if (!manifestJson.environments) {
            ISSUES.push('manifest.json: missing "environments" array');
            return;
        }
        
        const manifestEnvs = new Set();
        const declaredFiles = new Map();
        
        manifestJson.environments.forEach(env => {
            if (!env.path) {
                ISSUES.push(`Environment ${env.name}: missing "path" field`);
                return;
            }
            
            const fullPath = path.join(FIM_DIR, env.path);
            manifestEnvs.add(env.env || env.name);
            
            if (!fs.existsSync(fullPath)) {
                ISSUES.push(`File not found: ${fullPath}`);
                return;
            }
            
            const fileSize = fs.statSync(fullPath).size;
            declaredFiles.set(env.path, { size: fileSize, env: env.env || env.name });
            
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
            .filter(f => f.endsWith('.bin'))
            .map(f => `/${f}`);
        
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
        
        if (missingInManifest.length > 0) {
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
        console.log(`⚠ manifest.json not found at ${MANIFEST_PATH}`);
        console.log('Skipping WP-95 validation.\n');
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
