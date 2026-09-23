'use strict';

/**
 * WP-72: Validate that libdeps patches for AsyncTCP/ESPAsyncWebServer are applied.
 * 
 * Checks:
 * 1. Legacy patch snapshots remain available for source review.
 * 2. The version-aware ESPAsyncWebServer hook is part of every build.
 * 3. Installed 3.12.0 copies (when present) contain the dynamic buffer patch.
 * 4. Installed AsyncTCP 3.5.0 copies contain the inbound admission gate.
 * 5. platformio.ini still has the target-specific AsyncTCP connection limits.
 */

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.resolve(__dirname, '..');
const PATCHES_DIR = path.join(ROOT_DIR, 'patches', 'libdeps');
const PLATFORMIO_INI = path.join(ROOT_DIR, 'platformio.ini');
const PATCH_HOOK = path.join(ROOT_DIR, 'scripts', 'apply-libdeps-patches.py');
const CURRENT_WEB_SERVER_VERSION = '3.12.0';
const CURRENT_ASYNC_TCP_VERSION = '3.5.0';

const REQUIRED_PATCH_DIRS = ['AsyncTCP@3.4.10', 'ESPAsyncWebServer@3.11.1'];
const ISSUES = [];

function readText(filePath) {
    return fs.readFileSync(filePath, 'utf8');
}

function hasFileRetryContract(source) {
    const start = source.indexOf('size_t AsyncFileResponse::_fillBuffer');
    if (start < 0) return false;
    const end = source.indexOf('/*\n * Stream Response', start);
    const block = source.slice(start, end < 0 ? start + 1200 : end);
    return block.includes('_content.position() < _content.size()') &&
        block.includes('retry < 4') &&
        block.includes('return RESPONSE_TRY_AGAIN;');
}

function hasSendProgressContract(source) {
    return source.includes('account bytes accepted by AsyncTCP before a partial') &&
        source.includes('payloadlen += added_len;\n        if (added_len !=') &&
        source.includes('zero progress must not consume the finite send credit') &&
        source.includes('hasPendingSendBuffer') &&
        source.includes('writable TCP space is authoritative liveness evidence') &&
        source.includes('if (!len && !_in_flight_credit && tcpWindow > 0)') &&
        source.includes('(!hasPendingSendBuffer && responseBufferCap > tcpWindow)') &&
        source.includes('if (pcb_written) {') &&
        source.includes('if (payloadlen) {') &&
        !source.includes('_in_flight += payloadlen;\n    --_in_flight_credit');
}

function hasFinalBufferDrainContract(source) {
    return source.includes('ERR_CONTENT_LENGTH_MISMATCH under memory pressure') &&
        source.includes('if (_sendContentLength && _sentLength == _contentLength) {') &&
        source.includes('Do not mark RESPONSE_END here. The final bytes are staged') &&
        !source.includes('// it was last piece of content\n            _state = RESPONSE_END;');
}

function hasAckTimeoutRecoveryContract(header, source) {
    const ack = source.indexOf('void AsyncWebServerRequest::_onAck');
    const reset = source.indexOf('_ackTimeoutRecoveryAttempted = false;', ack);
    const timeout = source.indexOf('void AsyncWebServerRequest::_onTimeout');
    const attempted = source.indexOf('!_ackTimeoutRecoveryAttempted', timeout);
    const progress = source.indexOf('_response->_ack(this, 0, 0) > 0', attempted);
    const close = source.indexOf('_client->close();', progress);
    return header.includes('bool _ackTimeoutRecoveryAttempted = false;') &&
        ack >= 0 && reset > ack && timeout > reset && attempted > timeout &&
        progress > attempted && close > progress &&
        source.includes('_response && !_response->_finished()') &&
        source.includes('_client && _client->canSend()');
}

function hasAsyncTcpAdmissionContract(header, source) {
    const increment = source.indexOf('__atomic_fetch_add(&AsyncClient::_fastbee_accepted_count');
    const eventAllocation = source.indexOf('lwip_tcp_event_packet_t *e = new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ACCEPT, c}');
    const rollback = source.indexOf('c->_fastbee_release_accepted();', eventAllocation);
    const cleanup = source.indexOf('delete c;', rollback);
    const tcpError = source.indexOf('void AsyncTCP_detail::tcp_error');
    const tcpErrorRelease = source.indexOf('client->_fastbee_release_accepted();', tcpError);
    const tcpErrorAllocation = source.indexOf(
        'lwip_tcp_event_packet_t *e = new (std::nothrow) lwip_tcp_event_packet_t{LWIP_TCP_ERROR, client}',
        tcpError
    );
    const close = source.indexOf('int8_t AsyncClient::_close()');
    const closeRelease = source.indexOf('_fastbee_release_accepted();', close);
    return header.includes('_fastbee_accepted_count') &&
        header.includes('_fastbee_server_accepted') &&
        header.includes('void _fastbee_release_accepted();') &&
        source.includes('FastBee patch: reject a connection before allocating AsyncClient') &&
        source.includes('heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)') &&
        source.includes('heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)') &&
        source.includes('accepted >= CONFIG_ASYNC_TCP_MAX_CONNECTIONS') &&
        source.includes('void AsyncClient::_fastbee_release_accepted()') &&
        source.includes('__atomic_exchange_n(') &&
        source.includes('FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE') &&
        source.includes('_fastbee_alloc_error_event(client)') &&
        source.includes('_fastbee_release_error_event(evpkt)') &&
        source.includes('emergency pool exhausted') &&
        increment >= 0 && eventAllocation > increment &&
        rollback > eventAllocation && cleanup > rollback &&
        tcpError >= 0 && tcpErrorRelease > tcpError &&
        tcpErrorAllocation > tcpErrorRelease &&
        close >= 0 && closeRelease > close;
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

        const c6Flags = text.match(/\[esp32c6_runtime_flags\][\s\S]*?(?=\r?\n\[|$)/);
        if (!c6Flags || !c6Flags[0].includes('-DCONFIG_ASYNC_TCP_MAX_ACK_TIME=10000')) {
            ISSUES.push('platformio.ini: C6 runtime must allow a 10s ACK recovery window');
        } else {
            console.log('  ✓ platformio.ini: C6 ACK recovery window is 10s');
        }
        if (!hasFileRetryContract(responsesCpp)) {
            ISSUES.push('ESPAsyncWebServer legacy snapshot: incomplete AsyncFileResponse zero-read retry contract');
        } else {
            console.log('  ✓ ESPAsyncWebServer legacy snapshot: transient zero-read keeps the response open');
        }
        if (!hasSendProgressContract(responsesCpp)) {
            ISSUES.push('ESPAsyncWebServer legacy snapshot: unsafe send-credit accounting');
        } else {
            console.log('  ✓ ESPAsyncWebServer legacy snapshot: send credits follow actual TCP progress');
        }
        if (!hasFinalBufferDrainContract(responsesCpp)) {
            ISSUES.push('ESPAsyncWebServer legacy snapshot: final staging buffer can be closed early');
        } else {
            console.log('  ✓ ESPAsyncWebServer legacy snapshot: final staging buffer drains before close');
        }
        
        return true;
    } catch (err) {
        ISSUES.push(`platformio.ini: error reading - ${err.message}`);
        return false;
    }
}

function checkRuntimePatchHook() {
    let platformio;
    let hook;
    try {
        platformio = readText(PLATFORMIO_INI);
        hook = readText(PATCH_HOOK);
    } catch (err) {
        ISSUES.push(`runtime patch hook: unable to read - ${err.message}`);
        return;
    }

    if (!platformio.includes('pre:scripts/apply-libdeps-patches.py')) {
        ISSUES.push('platformio.ini: apply-libdeps-patches.py is not registered as a pre-build hook');
    } else {
        console.log('  ✓ platformio.ini: version-aware libdeps patch hook registered');
    }

    for (const marker of ['3.12.0', '3.5.0', 'kFallbackSizes',
        'FastBee patch: dynamic send buffer', 'zero-read retry', 'chunkCap',
        'return RESPONSE_TRY_AGAIN;', 'MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT',
        'zero progress must not consume the finite send credit',
        'account bytes accepted by AsyncTCP before a partial',
        'hasPendingSendBuffer',
        'writable TCP space is authoritative liveness evidence',
        'ERR_CONTENT_LENGTH_MISMATCH under memory pressure',
        'bool _ackTimeoutRecoveryAttempted = false;',
        '_response->_ack(this, 0, 0) > 0',
        'CONFIG_ASYNC_TCP_MAX_CONNECTIONS', '__atomic_exchange_n(',
        'FASTBEE_ASYNC_TCP_ERROR_EVENT_POOL_SIZE',
        '_fastbee_alloc_error_event(client)', '_fastbee_release_error_event(evpkt)',
        'client->_fastbee_release_accepted();', 'c->_fastbee_release_accepted();',
        'delete c;']) {
        if (!hook.includes(marker)) {
            ISSUES.push(`apply-libdeps-patches.py: missing marker ${marker}`);
        }
    }
    if (ISSUES.length === 0 || !ISSUES.some((issue) => issue.startsWith('apply-libdeps-patches.py:'))) {
        console.log('  ✓ apply-libdeps-patches.py: 3.12.0 fallback patch markers present');
    }

    // A clean checkout has no .pio directory, so installed-source checks are
    // conditional. When dependencies are present, do not allow a build report
    // to claim the hook ran while the actual source is still upstream.
    const libdepsRoot = path.join(ROOT_DIR, '.pio', 'libdeps');
    if (!fs.existsSync(libdepsRoot)) {
        console.log('  - .pio/libdeps absent: installed-source check deferred to the build hook');
        return;
    }
    let foundWebServer = 0;
    let checkedWebServer = 0;
    let foundAsyncTcp = 0;
    let checkedAsyncTcp = 0;
    for (const envName of fs.readdirSync(libdepsRoot)) {
        const libraryDir = path.join(libdepsRoot, envName, 'ESPAsyncWebServer');
        const metadataPath = path.join(libraryDir, 'library.json');
        if (fs.existsSync(metadataPath)) {
            foundWebServer++;
            let metadata;
            try {
                metadata = JSON.parse(readText(metadataPath));
            } catch (err) {
                ISSUES.push(`${envName}/ESPAsyncWebServer: invalid library.json - ${err.message}`);
                metadata = null;
            }
            if (metadata) {
                const version = String(metadata.version);
                if (version !== CURRENT_WEB_SERVER_VERSION) {
                    ISSUES.push(`${envName}/ESPAsyncWebServer v${version}: unsupported installed version`);
                } else {
                    checkedWebServer++;
                    const header = path.join(libraryDir, 'src', 'WebResponseImpl.h');
                    const source = path.join(libraryDir, 'src', 'WebResponses.cpp');
                    const requestHeader = path.join(libraryDir, 'src', 'ESPAsyncWebServer.h');
                    const requestSource = path.join(libraryDir, 'src', 'WebRequest.cpp');
                    if (![header, source, requestHeader, requestSource].every(fs.existsSync)) {
                        ISSUES.push(`${envName}/ESPAsyncWebServer: expected source files missing`);
                    } else {
                        const headerText = readText(header);
                        const sourceText = readText(source);
                        const requestHeaderText = readText(requestHeader);
                        const requestSourceText = readText(requestSource);
                        if (!headerText.includes('FastBee patch: dynamic send buffer') ||
                            !sourceText.includes('kFallbackSizes') ||
                            !sourceText.includes('chunkCap') ||
                            !hasFileRetryContract(sourceText) ||
                            !hasSendProgressContract(sourceText) ||
                            !hasFinalBufferDrainContract(sourceText) ||
                            !hasAckTimeoutRecoveryContract(requestHeaderText, requestSourceText)) {
                            ISSUES.push(`${envName}/ESPAsyncWebServer v${CURRENT_WEB_SERVER_VERSION}: runtime patch not applied`);
                        }
                    }
                }
            }
        }

        const asyncTcpDir = path.join(libdepsRoot, envName, 'AsyncTCP');
        const asyncTcpMetadata = path.join(asyncTcpDir, 'library.json');
        if (!fs.existsSync(asyncTcpMetadata)) continue;
        foundAsyncTcp++;
        let asyncMetadata;
        try {
            asyncMetadata = JSON.parse(readText(asyncTcpMetadata));
        } catch (err) {
            ISSUES.push(`${envName}/AsyncTCP: invalid library.json - ${err.message}`);
            continue;
        }
        const asyncVersion = String(asyncMetadata.version);
        if (asyncVersion !== CURRENT_ASYNC_TCP_VERSION) {
            ISSUES.push(`${envName}/AsyncTCP v${asyncVersion}: unsupported installed version`);
            continue;
        }
        checkedAsyncTcp++;
        const asyncHeader = path.join(asyncTcpDir, 'src', 'AsyncTCP.h');
        const asyncSource = path.join(asyncTcpDir, 'src', 'AsyncTCP.cpp');
        if (!fs.existsSync(asyncHeader) || !fs.existsSync(asyncSource)) {
            ISSUES.push(`${envName}/AsyncTCP: expected source files missing`);
            continue;
        }
        if (!hasAsyncTcpAdmissionContract(readText(asyncHeader), readText(asyncSource))) {
            ISSUES.push(`${envName}/AsyncTCP v${CURRENT_ASYNC_TCP_VERSION}: runtime admission patch not applied`);
        }
    }
    if (foundWebServer === 0) {
        console.log('  - no installed ESPAsyncWebServer copies: check deferred to the build hook');
    } else {
        console.log(`  ✓ installed ESPAsyncWebServer copies checked: ${checkedWebServer}/${foundWebServer}`);
    }
    if (foundAsyncTcp === 0) {
        console.log('  - no installed AsyncTCP copies: check deferred to the build hook');
    } else {
        console.log(`  ✓ installed AsyncTCP copies checked: ${checkedAsyncTcp}/${foundAsyncTcp}`);
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

    // 3. Check the current dependency version is patched in the build path
    console.log('\n[3] Verifying runtime dependency patch hook...');
    checkRuntimePatchHook();
    
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
