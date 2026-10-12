'use strict';

// Fast, host-only checks for regressions that are difficult to catch on a
// single board: 4MB layout safety, no split source graph, and Modbus buffer
// capacities that can represent a full 256-byte response.
const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const read = (file) => fs.readFileSync(path.join(root, file), 'utf8');
const failures = [];

const partition = read('partitions/4MB.csv');
const rows = partition.split(/\r?\n/)
    .filter((line) => line.trim() && !line.trim().startsWith('#'))
    .map((line) => line.split(',').map((field) => field.trim()))
    .filter((fields) => fields.length >= 5)
    .map(([name, type, subtype, offset, size]) => ({
        name, type, subtype,
        offset: Number.parseInt(offset, 16),
        size: Number.parseInt(size, 16)
    }));

const sortedRows = [...rows].sort((a, b) => a.offset - b.offset);
for (let i = 1; i < sortedRows.length; i += 1) {
    if (sortedRows[i - 1].offset + sortedRows[i - 1].size > sortedRows[i].offset) {
        failures.push(`4MB partition overlap: ${sortedRows[i - 1].name}/${sortedRows[i].name}`);
    }
}
if (sortedRows.some((row) => row.offset + row.size > 0x400000)) {
    failures.push('4MB partition exceeds flash capacity');
}
if (rows.some((row) => row.type === 'app' && row.subtype === 'ota_1')) {
    failures.push('4MB partition must not contain ota_1');
}
if (!rows.some((row) => row.name === 'spiffs' && row.type === 'data')) {
    failures.push('4MB partition is missing the LittleFS-compatible spiffs label');
}

const modbusHeader = read('include/protocols/ModbusHandler.h');
const modbusSource = read('src/protocols/ModbusHandler.cpp');
if (/rawResp,\s*uint8_t\s+rawCap/.test(modbusHeader) || /rawResp,\s*uint8_t\s+rawCap/.test(modbusSource)) {
    failures.push('Modbus rawCap must use size_t, not uint8_t');
}
if (!/if\s*\(respIdx\s*>\s*rawCap\)/.test(modbusSource) ||
    !/memcpy\(rawResp,\s*respBuf,\s*respIdx\)/.test(modbusSource) ||
    !/rawLen\s*=\s*respIdx/.test(modbusSource)) {
    failures.push('Modbus response copy must preserve size_t capacity');
}

const memoryBudget = read('include/core/MemoryBudget.h');
const mainSource = read('src/main.cpp');
const healthMonitor = read('src/systems/HealthMonitor.cpp');
const frameworkSource = read('src/core/FastBeeFramework.cpp');
const authRouteSource = read('src/network/handlers/AuthRouteHandler.cpp');
const runtimeFeatureSource = read('src/core/RuntimeFeatureManager.cpp');
const protocolManagerSource = read('src/protocols/ProtocolManager.cpp');
const protocolManagerHeader = read('include/protocols/ProtocolManager.h');
const protocolRouteSource = read('src/network/handlers/ProtocolRouteHandler.cpp');
const periphExecManagerSource = read('src/core/PeriphExecManager.cpp') + '\n' + read('src/core/PeriphExecWorkers.cpp');
const deployConfigBundle = read('scripts/deploy-config-bundle.ps1');
const healthMonitorHeader = read('include/systems/HealthMonitor.h');
const healthMonitorSource = read('src/systems/HealthMonitor.cpp');
const workerPoolHeader = read('include/core/PeriphExecWorkerPool.h');
const workerPoolSource = read('src/core/PeriphExecWorkerPool.cpp');
const periphExecHeader = read('include/core/PeriphExecManager.h');
if (!/heap_caps_get_free_size\(MALLOC_CAP_INTERNAL\s*\|\s*MALLOC_CAP_8BIT\)/.test(memoryBudget) ||
    !/heap_caps_get_largest_free_block\(MALLOC_CAP_INTERNAL\s*\|\s*MALLOC_CAP_8BIT\)/.test(memoryBudget)) {
    failures.push('MemoryBudget dram8 helpers must use internal 8-bit DRAM caps');
}
if (!/MemoryBudget::dram8Free\(\)/.test(mainSource)) {
    failures.push('main loop telemetry must use MemoryBudget::dram8Free()');
}
if (!/MemoryBudget::dram8Free\(\)/.test(frameworkSource) ||
    !/MemoryBudget::dram8Largest\(\)/.test(frameworkSource)) {
    failures.push('status telemetry must use MemoryBudget DRAM helpers');
}
if (!/MemoryBudget::dram8Free\(\)/.test(healthMonitor) ||
    !/MemoryBudget::dram8Largest\(\)/.test(healthMonitor)) {
    failures.push('HealthMonitor must use MemoryBudget DRAM helpers');
}
if (!/CHIP_DUAL_CORE[\s\S]*xSemaphoreCreateCounting\(2, 2\)/.test(authRouteSource) ||
    !/xSemaphoreCreateCounting\(1, 1\)/.test(authRouteSource)) {
    failures.push('Auth login slots must serialize PBKDF2 on single-core chips');
}
if (!authRouteSource.includes('LOGIN_TASK_STACK_ALLOCATOR_OVERHEAD_BYTES = 256U') ||
    !authRouteSource.includes('LOGIN_TASK_MIN_REMAINING_DRAM_BYTES = 12288U') ||
    !authRouteSource.includes('largestBlock < requiredBlock || freeDram < requiredFree') ||
    authRouteSource.includes('xTaskCreatePinnedToCoreWithCaps') ||
    authRouteSource.includes('LOGIN_TASK_STACK_HEADROOM_BYTES')) {
    failures.push('Login admission must keep flash-accessing task stacks in internal DRAM');
}
if (!/const bool active = shouldEnable\(st\.meta\.key\)/.test(runtimeFeatureSource) ||
    !/if \(active\) \{[\s\S]*activeFlashKB/.test(runtimeFeatureSource)) {
    failures.push('Feature status resource summary must count effective active features, not only userEnabled');
}
if ((protocolManagerSource.match(/new \(std::nothrow\) MQTTClient\(\)/g) || []).length < 2 ||
    !/MQTT restart allocation failed \(nothrow\)/.test(protocolManagerSource) ||
    !/MQTT deferred restart allocation failed \(nothrow\)/.test(protocolManagerSource)) {
    failures.push('MQTT rebuild paths must use non-throwing allocation guards');
}
if ((protocolManagerSource.match(/new \(std::nothrow\) ModbusHandler\(\)/g) || []).length < 2 ||
    !protocolManagerSource.includes('Modbus allocation failed (nothrow)') ||
    !protocolManagerSource.includes('Modbus init allocation failed (nothrow)')) {
    failures.push('Modbus startup/recovery paths must use non-throwing allocation guards');
}
if (!protocolManagerHeader.includes('isModbusRestartPending') ||
    !healthMonitorHeader.includes('_modbusRestorePendingForMemory') ||
    !healthMonitorSource.includes('Modbus handler confirmed running; cleared degraded state') ||
    !healthMonitorSource.includes('reconcilePendingMemoryRecovery')) {
    failures.push('Modbus memory recovery must wait for confirmed handler startup');
}
if (!periphExecManagerSource.includes('Skip heavy rule during memory pressure') ||
    !periphExecManagerSource.includes('const bool requiresWorker = ruleRequiresWorkerPool(rule)') ||
    !periphExecManagerSource.includes('monitor->isMemorySevere() && requiresWorker') ||
    !periphExecManagerSource.includes('if (shouldAvoidSyncFallback(rule)) return true;')) {
    failures.push('PeriphExec must reject heavy event rules during severe memory pressure');
}
if (!/bool\s+shutdown\(uint32_t\s+timeoutMs\s*=\s*10000\)/.test(workerPoolHeader) ||
    !workerPoolSource.includes('keeping pool alive') ||
    !workerPoolSource.includes('cancelWorkerJob')) {
    failures.push('Worker pool shutdown must be retryable and release queued contexts');
}
if (!workerPoolHeader.includes(
        '(CHIP_IS_CLASSIC_ESP32 || CHIP_IS_ESP32C3) ? 1 : 2')) {
    failures.push('PeriphExec worker policy must use one worker on C3/classic and retain two on C6/S3');
}
if (!periphExecHeader.includes('void cancelWorkerJob(AsyncExecContext* ctx);') ||
    !periphExecManagerSource.includes('old worker still active')) {
    failures.push('PeriphExec memory recovery must retain and retry a timed-out worker pool');
}
const workerAdmissionStart = periphExecManagerSource.indexOf('static bool hasWorkerPoolMemoryHeadroom');
const workerResumeStart = periphExecManagerSource.indexOf('bool PeriphExecManager::resumeWorkersAfterMemory()');
const workerResumeEnd = periphExecManagerSource.indexOf('void PeriphExecManager::cancelWorkerJob', workerResumeStart);
const workerResumeBlock = workerResumeStart >= 0 && workerResumeEnd > workerResumeStart
    ? periphExecManagerSource.slice(workerResumeStart, workerResumeEnd)
    : '';
if (workerAdmissionStart < 0 ||
    !periphExecManagerSource.includes('MemoryBudget::dram8Free()') ||
    !periphExecManagerSource.includes('MemoryBudget::dram8Largest()') ||
    !/webReserveAfterWorker\s*=\s*\(CHIP_IS_CLASSIC_ESP32\s*&&\s*FASTBEE_USE_PSRAM\s*==\s*0\)\s*\?\s*25600U/.test(periphExecManagerSource) ||
    !periphExecManagerSource.includes('WiFi.status() != WL_CONNECTED') ||
    !workerResumeBlock.includes('hasWorkerPoolMemoryHeadroom(true') ||
    workerResumeBlock.indexOf('hasWorkerPoolMemoryHeadroom(true') >
        workerResumeBlock.indexOf('new (std::nothrow) PeriphExecWorkerPool')) {
    failures.push('PeriphExec worker recovery must pass the startup 8-bit DRAM/Web-reserve admission check before allocation');
}
if (!healthMonitorSource.includes('pendingNormalRecovery') ||
    !healthMonitorSource.includes('NORMAL restore pending (will retry)')) {
    failures.push('Memory recovery must retry services that were not ready on the first NORMAL sample');
}
const handlerUtilsSource = read('include/network/handlers/HandlerUtils.h');
const healthMonitorHeaderSource = read('include/systems/HealthMonitor.h');
if (!healthMonitorHeaderSource.includes('arePeriphExecWorkersSuspendedForMemory') ||
    !healthMonitorHeaderSource.includes('isPeriphExecWorkerSuspensionPending') ||
    !handlerUtilsSource.includes('periphExecWorkerSuspended') ||
    !handlerUtilsSource.includes('periphExecWorkerSuspensionPending')) {
    failures.push('Compact memory API must expose PeriphExec worker suspension state');
}
if (!healthMonitorSource.includes('PeriphExec workers fully suspended after retry') ||
    !healthMonitorSource.includes('PeriphExec worker shutdown still pending') ||
    !healthMonitorSource.includes('periphExecWorkerSuspensionPending') ||
    !healthMonitorSource.includes('PeriphExec worker restore still pending') ||
    !healthMonitorSource.includes('workerSuspended')) {
    failures.push('Memory recovery must retry and expose pending PeriphExec worker shutdown');
}
if (!healthMonitorSource.includes('historical plaintext downgrade blocked') ||
    !/setRuntimeDisabled\(\s*"mqtts"/.test(healthMonitorSource)) {
    failures.push('Historical plaintext downgrade must be blocked and visible after reboot');
}
if (!healthMonitorSource.includes('disabled_by_memory') ||
    !healthMonitorSource.includes('restored persisted MQTT disable') ||
    !healthMonitorSource.includes('clearMqttMemoryOverrideState')) {
    failures.push('MQTT memory-disable marker must persist, restore, and be clearable by an explicit user save');
}
if (!healthMonitorHeaderSource.includes('_mqttRestorePendingForMemory') ||
    !healthMonitorSource.includes('MQTT restart could not be queued; keeping degraded state') ||
    !healthMonitorSource.includes('MQTT restart queued and client ready; cleared degraded state') ||
    !healthMonitorSource.includes('_mqttRestoreRetryAtMs') ||
    !handlerUtilsSource.includes('mqttRestorePending')) {
    failures.push('MQTT memory recovery must retain a pending state and expose retry status until restart succeeds');
}
if ((healthMonitorSource.match(/restartQueued && restored && !restored->isStopped\(\)/g) || []).length < 2) {
    failures.push('MQTT recovery must verify a ready client in both retry and NORMAL restore paths');
}
if (!healthMonitorSource.includes('configuration preserved, no plaintext fallback') ||
    !healthMonitorSource.includes('restoreMqttsAfterMemory()') ||
    healthMonitorSource.includes('doc["mqtt"]["scheme"] = "mqtt"') ||
    healthMonitorSource.includes('doc["mqtt"]["enabled"] = false')) {
    failures.push('Memory protection must pause/retry TLS without rewriting user scheme/port/enabled or permitting plaintext fallback');
}
if (!protocolRouteSource.includes('remove("disabled_by_memory")') ||
    !protocolRouteSource.includes('remove("downgraded_by_memory")') ||
    !protocolRouteSource.includes('clearMqttMemoryOverrideState()')) {
    failures.push('Explicit MQTT Web saves must clear automatic memory override markers and runtime state');
}

const platformio = read('platformio.ini');
if (!/\$profile\s+-eq\s+"classic-no-psram"[\s\S]*\$optionalTypes\s*\+=\s*38/.test(deployConfigBundle) ||
    !/Set-PropertyValue\s+\$network\s+"enableMDNS"\s+\$false/.test(deployConfigBundle) ||
    !/Set-PropertyValue\s+\$network\s+"autoFailover"\s+\$false/.test(deployConfigBundle) ||
    !/keep asynchronous STA recovery on classic ESP32/.test(deployConfigBundle)) {
    failures.push('classic-no-psram deployment must shed DHT/mDNS and keep STA recovery instead of AP failover');
}
for (const env of ['esp32-F4R0', 'esp32c3-F4R0', 'esp32c6-F4R0']) {
    const sectionMatch = platformio.match(new RegExp(`\\[env:${env}\\]([\\s\\S]*?)(?=\\n\\[|$)`));
    if (!sectionMatch || !/board_build\.partitions\s*=\s*partitions\/4MB\.csv/.test(sectionMatch[1])) {
        failures.push(`${env} must use partitions/4MB.csv`);
    }
    if (sectionMatch && !/-DFASTBEE_USE_PSRAM=0/.test(sectionMatch[1])) {
        failures.push(`${env} must explicitly disable PSRAM`);
    }
    if (sectionMatch && (!/-DFASTBEE_ENABLE_ETHERNET=0/.test(sectionMatch[1]) ||
        !/-DFASTBEE_ENABLE_CELLULAR=0/.test(sectionMatch[1]) ||
        !/build_src_filter\s*=\s*\$\{wifi_only_src_filter\.build_src_filter\}/.test(sectionMatch[1]))) {
        failures.push(`${env} must exclude Ethernet/4G code through flags and the Wi-Fi-only source filter`);
    }
}

if (!healthMonitorSource.includes('suspendOptionalForMemory()') ||
    !healthMonitorSource.includes('setRuntimeDisabled("fileLogging"') ||
    !healthMonitorSource.includes('setRuntimeDisabled("modbus"') ||
    !healthMonitorSource.includes('setRuntimeDisabled("ruleScript"') ||
    !healthMonitorSource.includes('setRuntimeDisabled("commandScript"')) {
    failures.push('SEVERE memory recovery must suspend optional peripherals and expose runtime-disabled features');
}

// Stable devices retain about a 10KB internal largest block while the hardest
// service allocations need 2.9-4KB. Keep the shared largest-block levels below
// that stable band, including the C3/classic no-PSRAM overrides, so a normal
// login cannot permanently suspend Modbus/PeriphExec.
if (!(healthMonitor.includes('#elif defined(CONFIG_IDF_TARGET_ESP32C3)') ||
      healthMonitor.includes('#elif CHIP_IS_ESP32C3')) ||
    !memoryBudget.includes('GUARD_WARN_LARGEST_BLOCK = 8192U') ||
    !memoryBudget.includes('GUARD_SEVERE_LARGEST_BLOCK = 4096U') ||
    !memoryBudget.includes('GUARD_CRITICAL_LARGEST_BLOCK = 3072U') ||
    !healthMonitor.includes('guardCriticalLargestCache = FastBee::MemoryBudget::GUARD_CRITICAL_LARGEST_BLOCK') ||
    !healthMonitor.includes('guardSevereLargestCache = FastBee::MemoryBudget::GUARD_SEVERE_LARGEST_BLOCK') ||
    !healthMonitor.includes('guardWarnLargestCache = FastBee::MemoryBudget::GUARD_WARN_LARGEST_BLOCK')) {
    failures.push('memory guard largest-block bands must match measured service allocation floors');
}

if (failures.length > 0) {
    console.error('Memory regression checks failed:');
    failures.forEach((failure) => console.error(`- ${failure}`));
    process.exit(1);
}

console.log('Memory regression checks OK');
