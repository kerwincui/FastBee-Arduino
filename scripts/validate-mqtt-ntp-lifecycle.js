'use strict';

const fs = require('fs');
const path = require('path');

const ROOT_DIR = path.join(__dirname, '..');
const failures = [];

function read(relPath) {
    return fs.readFileSync(path.join(ROOT_DIR, relPath), 'utf8');
}

function fail(message) {
    failures.push(message);
    console.error(`FAIL ${message}`);
}

function assert(condition, message) {
    if (!condition) fail(message);
}

function snippetBetween(source, startNeedle, endNeedle) {
    const start = source.indexOf(startNeedle);
    if (start < 0) return '';
    const end = source.indexOf(endNeedle, start + startNeedle.length);
    return source.slice(start, end < 0 ? undefined : end);
}

function snippetAround(source, needle, before = 300, after = 500) {
    const index = source.indexOf(needle);
    if (index < 0) return '';
    return source.slice(Math.max(0, index - before), index + after);
}

function checkNtpLifecycle() {
    const framework = read('src/core/FastBeeFramework.cpp');
    const scheduleSnippet = snippetAround(framework, 'NTP sync scheduled');
    const successSnippet = snippetAround(framework, 'LOG_INFOF("[NTP] Sync successful');
    const scheduleCondition = snippetAround(framework, 'if (!framework->ntpSynced', 80, 280);

    assert(scheduleSnippet.includes('framework->ntpSyncPending = true'), 'NTP boot scheduler should queue a sync');
    assert(!/ntpSynced\s*=\s*true/.test(scheduleSnippet), 'NTP scheduler must not mark sync complete before time is valid');
    assert(scheduleCondition.includes('!framework->ntpSyncPending'), 'NTP scheduler should not requeue while pending');
    assert(scheduleCondition.includes('!framework->ntpSyncStarted'), 'NTP scheduler should not requeue while started');
    assert(scheduleCondition.includes('framework->ntpRetryCount == 0'), 'NTP scheduler should not spin after retry exhaustion');

    assert(/ntpSynced\s*=\s*true/.test(successSnippet), 'NTP success branch should mark sync complete');
    assert(successSnippet.includes('EventType::EVENT_NTP_SYNCED'), 'NTP success branch should trigger EVENT_NTP_SYNCED');
    assert(/triggerEvent\(\s*EventType::EVENT_NTP_SYNCED\s*,\s*timeStr\s*\)/s.test(successSnippet), 'NTP synced event should include formatted time');
}

function checkMqttLifecycle() {
    const route = read('src/network/handlers/MqttRouteHandler.cpp');
    const statusHandler = snippetBetween(
        route,
        'void MqttRouteHandler::handleGetMqttStatus',
        'void MqttRouteHandler::handleMqttReconnect'
    );

    assert(route.includes('loadMqttStatusConfig'), 'MQTT status should read persisted MQTT config');
    assert(route.includes('tryAutoStartMqttForStatus'), 'MQTT status auto-start helper missing');
    assert(route.includes('restartMQTTDeferred'), 'MQTT status should use deferred restart instead of blocking test connect');
    assert(statusHandler.includes('tryAutoStartMqttForStatus'), 'MQTT status handler should invoke auto-start helper');
    assert(statusHandler.indexOf('tryAutoStartMqttForStatus') < statusHandler.indexOf('JsonDocument doc'), 'MQTT auto-start should happen before status response is built');
    assert(statusHandler.includes('data["autoStartAttempted"]'), 'MQTT status response should expose autoStartAttempted');
    assert(statusHandler.includes('data["autoStartStarted"]'), 'MQTT status response should expose autoStartStarted');
    assert(statusHandler.includes('data["connecting"]'), 'MQTT status response should expose connecting state');
    assert(statusHandler.includes('configuredServer'), 'MQTT status should return configured server when runtime client is missing');

    const ui = read('web-src/modules/runtime/protocol/mqtt-config.js');
    assert(ui.includes('d.connecting'), 'MQTT UI should read connecting state');
    assert(ui.includes('d.autoStartStarted'), 'MQTT UI should read autoStartStarted state');
    assert(ui.includes('mqtt-status-connecting'), 'MQTT UI should render connecting badge class');
}

checkNtpLifecycle();
checkMqttLifecycle();

if (failures.length > 0) {
    console.error(`\nMQTT/NTP lifecycle validation failed: ${failures.length} issue(s)`);
    process.exit(1);
}

console.log('MQTT/NTP lifecycle validation passed');
