'use strict';

/**
 * WP-72: Validate that libdeps patches for AsyncTCP/ESPAsyncWebServer are applied.
 * 
 * Checks:
 * 1. patches/libdeps exists with AsyncTCP and ESPAsyncWebServer folders
 * 2. Each patch folder contains at least one modified source file (.cpp/.h)
 * 3. platformio.ini has -DCONFIG_* overrides for these libraries
 * 4. If libdeps exists, they must be loaded (PIO will use them if present)
 */

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const PATCHES_DIR = path.join(ROOT_DIR, 'patches', 'libdeps');
const PLATFORMIO_INI = path.join(ROOT_DIR, 'platformio.ini');

const REQUIRED_PATCH_DIRS = ['AsyncTCP@3.4.10', 'ESPAsyncWebServer@3.11.1'];
const ISSUES = [];

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function checkPatchDir(patchName) {
    const dirPath = path.join(PATCHES_DIR, patchName);
    if (!fs.existsSync(dirPath)) {
        ISSUES.push(`patches/libdeps/${patchName}: directory missing`);
        return false;
    }
    
    // Count source files in subdirectories
    let cppCount = 0;
    let hCount = 0;
    
    function walk(dir) {
        if (!fs.existsSync(dir)) return;
        const entries = fs.readdirSync(dir, { withFileTypes: true });
        entries.forEach(entry => {
            if (entry.isDirectory()) {
                walk(path.join(dir, entry.name));
            } else if (entry.isFile()) {
                if (entry.name.endsWith('.cpp')) cppCount++;
                else if (entry.name.endsWith('.h')) hCount++;
            }
        });
    }
    
    try {
        walk(dirPath);
        
        if (cppCount === 0 && hCount === 0) {
            ISSUES.push(`${patchName}: no .cpp or .h files found in patches/libdeps`);
            return false;
        }
        
        console.log(`  ✓ ${patchName}: ${cppCount} .cpp + ${hCount} .h files`);
        return true;
    } catch (err) {
        ISSUES.push(`${patchName}: error reading patch directory - ${err.message}`);
        return false;
    }
}

function checkPlatformioOverrides() {
    try {
        const text = readText(PLATFORMIO_INI);
        
        // Check for CONFIG_* overrides
        const configMatches = text.match(/-DCONFIG_ASYNC_TCP_/g) || [];

        if (configMatches.length < 3) {
            ISSUES.push(`platformio.ini: expected >=3 -DCONFIG_ASYNC_TCP_ overrides, found ${configMatches.length}`);
        } else {
            console.log(`  ✓ platformio.ini: ${configMatches.length} AsyncTCP CONFIG overrides`);
        }

        // ESPAsyncWebServer 补丁方案已演进（2026-08）：不再依赖 -DCONFIG_ESPASYNCWEBSERVER_*，
        // 改为补丁内 #ifndef CONFIG_LWIP_TCP_MSS/WND 三段式回退（sdkconfig.defaults 提供 WND 默认值）。
        // 守卫改为验证补丁文件包含该派生链，确保缓冲尺寸随 lwIP 参数而非写死常量。
        const patchDir = path.join(ROOT_DIR, 'patches', 'libdeps', 'ESPAsyncWebServer@3.11.1', 'src');
        const implHeader = readText(path.join(patchDir, 'WebResponseImpl.h'));
        if (!implHeader.includes('#ifndef CONFIG_LWIP_TCP_MSS')) {
            ISSUES.push('ESPAsyncWebServer patch: WebResponseImpl.h missing CONFIG_LWIP_TCP_MSS derivation');
        } else {
            console.log('  ✓ ESPAsyncWebServer patch: buffer size derives from CONFIG_LWIP_TCP_MSS');
        }
        const responsesCpp = readText(path.join(patchDir, 'WebResponses.cpp'));
        if (!responsesCpp.includes('#ifndef CONFIG_LWIP_TCP_WND_DEFAULT')) {
            ISSUES.push('ESPAsyncWebServer patch: WebResponses.cpp missing CONFIG_LWIP_TCP_WND_DEFAULT derivation');
        } else {
            console.log('  ✓ ESPAsyncWebServer patch: window size derives from CONFIG_LWIP_TCP_WND_DEFAULT');
        }
        
        return true;
    } catch (err) {
        ISSUES.push(`platformio.ini: error reading - ${err.message}`);
        return false;
    }
}

function main() {
    console.log('\n[WP-72] Checking third-party library patches...\n');
    
    // 1. Check patch directories exist
    console.log('[1] Verifying patch directories...');
    REQUIRED_PATCH_DIRS.forEach(dirName => {
        checkPatchDir(dirName);
    });
    
    // 2. Check platformio.ini has CONFIG overrides
    console.log('\n[2] Verifying platformio.ini CONFIG overrides...');
    checkPlatformioOverrides();
    
    // Summary
    console.log('\n' + '='.repeat(50));
    if (ISSUES.length > 0) {
        console.error(`WP-72 validation FAILED: ${ISSUES.length} issue(s)`);
        ISSUES.forEach(issue => console.error(`  - ${issue}`));
        process.exit(1);
    } else {
        console.log('WP-72 PASSED: AsyncTCP/ESPAsyncWebServer patches verified');
        process.exit(0);
    }
}

main();
