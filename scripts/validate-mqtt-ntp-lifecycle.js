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

// Extract a complete JavaScript function/object method without relying on a
// byte-length window. The status renderer has grown past the old probe size;
// truncating it caused false failures even when the checked branches existed.
function balancedBlock(source, needle) {
    const start = source.indexOf(needle);
    if (start < 0) return '';
    const open = source.indexOf('{', start);
    if (open < 0) return '';

    let depth = 0;
    let quote = '';
    let escaped = false;
    let lineComment = false;
    let blockComment = false;

    for (let i = open; i < source.length; i++) {
        const ch = source[i];
        const next = source[i + 1];

        if (lineComment) {
            if (ch === '\n' || ch === '\r') lineComment = false;
            continue;
        }
        if (blockComment) {
            if (ch === '*' && next === '/') {
                blockComment = false;
                i++;
            }
            continue;
        }
        if (quote) {
            if (escaped) escaped = false;
            else if (ch === '\\') escaped = true;
            else if (ch === quote) quote = '';
            continue;
        }
        if (ch === '/' && next === '/') {
            lineComment = true;
            i++;
            continue;
        }
        if (ch === '/' && next === '*') {
            blockComment = true;
            i++;
            continue;
        }
        if (ch === '\'' || ch === '"' || ch === '`') {
            quote = ch;
            continue;
        }
        if (ch === '{') depth++;
        if (ch === '}' && --depth === 0) return source.slice(start, i + 1);
    }
    return '';
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
    assert(statusHandler.includes('if (autoStartAttempted)'), 'MQTT status must refresh the client pointer after failed deferred auto-start');
    assert(statusHandler.includes('data["connecting"]'), 'MQTT status response should expose connecting state');
    assert(statusHandler.includes('configuredServer'), 'MQTT status should return configured server when runtime client is missing');
    // Failure diagnostics must be available without parsing serial output.
    assert(statusHandler.includes('networkConnected'), 'MQTT status should expose active network connectivity separately from internetAvailable');
    assert(statusHandler.includes('lastErrorClass'), 'MQTT status should expose a stable failure class');
    assert(statusHandler.includes('lastErrorMessage'), 'MQTT status should expose a readable failure message');
    assert(statusHandler.includes('statusReason'), 'MQTT status should expose a user-facing status reason');
    assert(statusHandler.includes('data["stoppedByMemory"]'), 'MQTT status should expose temporary memory-stop state');
    assert(statusHandler.includes('MQTT_STATUS_STOPPED_BY_MEMORY'), 'MQTT status should expose a stable temporary memory-stop code');
    assert(statusHandler.includes('isLastMqttsTlsMemoryFailure'), 'MQTT status should distinguish TLS memory failures from network failures');
    assert(statusHandler.includes('autoStartAttempted && !autoStartStarted'), 'MQTT status should report deferred auto-start failures');
    assert(statusHandler.includes('MQTT_CLIENT_INIT_FAILED'), 'MQTT status should expose a stable client-init failure name');
    assert(statusHandler.includes('lastErrorClass"] = "resource"'), 'MQTT auto-start failure should use the resource error class');
    assert(route.includes('case -11: return "MQTT_CLIENT_INIT_FAILED"'), 'MQTT error-name mapping should include client-init failures');
    assert(route.includes('MQTT_STATUS_CONFIG_ERROR'), 'MQTT status should expose a non-OK code for missing/invalid config');
    assert(route.includes('MQTT_CONFIG_INVALID'), 'MQTT status should expose a stable config failure name');
    assert(route.includes('doc["success"] = deferredOk'), 'MQTT reconnect should expose rebuild failure in the response envelope');
    assert(route.includes('data["failureKnown"] = true'), 'MQTT reconnect failure should include a stable failure marker');
    assert(route.includes('data["disabledByMemory"] = true'), 'MQTT reconnect should expose persisted memory-disable state');
    assert(route.indexOf('mqttDisabledByMemory') < route.indexOf('if (mqtt && mqtt->getIsConnected())'), 'MQTT reconnect must check memory-disable marker before connected fast path');

    const mqttClient = read('src/protocols/MQTTClient.cpp');
    const notifyBlock = snippetBetween(mqttClient, 'void MQTTClient::_notifyStatusChange()', 'MqttTopicType MQTTClient::getTopicTypeByPath');
    assert(notifyBlock.includes('lastErrorCode'), 'MQTT SSE status should expose the structured error code');
    assert(notifyBlock.includes('lastErrorMessage'), 'MQTT SSE status should expose a readable failure message');
    assert(notifyBlock.includes('statusReason'), 'MQTT SSE status should expose a user-facing reason');
    assert(notifyBlock.includes('networkConnected'), 'MQTT SSE status should expose network connectivity context');
    assert(notifyBlock.includes('statusNet->isNetworkConnected()'),
        'MQTT SSE status must use the active network manager for Ethernet/4G/WiFi connectivity');
    assert(!notifyBlock.includes('const bool networkConnected = _externalClient || WiFi.status() == WL_CONNECTED'),
        'MQTT SSE status must not infer connectivity from transport pointer or WiFi status alone');
    assert(notifyBlock.includes('stoppedByMemory'), 'MQTT SSE status should expose temporary memory-stop state');
    assert(notifyBlock.includes('MQTT_STOPPED_BY_MEMORY'), 'MQTT SSE status should expose a stable temporary memory-stop name');
    assert(mqttClient.includes('case -14: return "MQTT_STOPPED_BY_MEMORY"'), 'MQTT SSE error mapping should include temporary memory-stop failures');
    assert(route.includes('return !server.isEmpty() && port > 0 && port <= 65535'), 'MQTT status config loader should reject enabled sections with an empty broker or invalid port');
    assert(statusHandler.includes('if (hasValidConfig && mqttConfigLoaded)'), 'MQTT status must not trust a stale in-memory client after config reload failure');
    assert(route.includes('MQTT 配置未加载或格式无效，请检查 Broker 地址和端口'), 'MQTT reconnect should explain malformed broker configuration');

    // 2025-06 修复: tryAutoStartMqttForStatus 应在 MQTT 已停止时也能触发重启
    const autoStartFn = snippetBetween(route, 'static bool tryAutoStartMqttForStatus', 'static String buildEncryptedPasswordForMqttTest');
    assert(autoStartFn.includes('mqtt->getIsConnected()'), 'tryAutoStartMqttForStatus should check MQTT connection state');
    assert(autoStartFn.includes('mqtt->isStopped()'), 'tryAutoStartMqttForStatus should check MQTT stopped state');
    assert(autoStartFn.includes('needsStart'), 'tryAutoStartMqttForStatus should use needsStart variable for combined condition');
    assert(autoStartFn.includes('isMqttStoppedForMemory()') &&
           autoStartFn.includes('isMqttDisabledForMemory()') &&
           autoStartFn.includes('isStoppedByMemory()') &&
           autoStartFn.includes('isDisabledByMemory()'),
        'MQTT status auto-start must not rebuild a client while memory protection is active');

    const ui = read('web-src/modules/runtime/protocol/mqtt-config.js');
    assert(ui.includes('d.connecting'), 'MQTT UI should read connecting state');
    assert(ui.includes('d.autoStartStarted'), 'MQTT UI should read autoStartStarted state');
    assert(ui.includes('mqtt-status-connecting'), 'MQTT UI should render connecting badge class');

    // 2025-06 修复: MQTT 启用后但未初始化时应显示"初始化中"而非"未初始化"
    // 当前实现使用 else-if 链：!d.enabled 先于 !d.initialized 检查，确保未启用时显示"未连接"，启用但未初始化时显示"初始化中"
    assert(ui.includes('!d.enabled'), 'MQTT UI should check !d.enabled to short-circuit when MQTT is disabled');
    assert(ui.includes('!d.initialized'), 'MQTT UI should check !d.initialized to show initializing state');

    // 2025-06 修复: _loadMqttStatus 不应在 401/503 时停止轮询或显示“未授权”
    const loadStatusFn = ui.match(/_loadMqttStatus\(\)\s*\{[\s\S]{0,4200}/);
    assert(loadStatusFn, '_loadMqttStatus function should exist');
    assert(!loadStatusFn[0].includes('_stopMqttStatusPolling'), '_loadMqttStatus must NOT stop polling on transient 401/503');
    assert(!loadStatusFn[0].includes("'未授权'"), '_loadMqttStatus must NOT show 未授权 badge during Governor cooldown');
    assert(loadStatusFn[0].includes('isPageAbortError'), '_loadMqttStatus must ignore page-navigation AbortError');
    assert(loadStatusFn[0].includes('_handled'), '_loadMqttStatus must not overwrite errors already handled by request-governor');
    assert(loadStatusFn[0].includes('_setMqttFailureStatus'), '_loadMqttStatus must update the badge for handled HTTP failures');
    assert(ui.includes("'-12': 'MQTT 配置未加载或格式无效，请检查网络配置'"), 'MQTT UI should map config status failures');

    // 2025-06 修复: _loadMqttStatus 应追踪连续失败次数
    assert(loadStatusFn[0].includes('_mqttApiFailCount'), '_loadMqttStatus should track API failure count');
    assert(loadStatusFn[0].includes('_handleMqttApiFail'), '_loadMqttStatus should call _handleMqttApiFail on failure');
    assert(loadStatusFn[0].includes('_mqttStatusRequestSeq'), '_loadMqttStatus must sequence requests so stale callbacks cannot update the UI');
    assert(loadStatusFn[0].includes('requestSeq !== self._mqttStatusRequestSeq'), '_loadMqttStatus must ignore stale success and failure callbacks');
    assert(loadStatusFn[0].includes('clearTimeout(self._mqttRetryTimer)'), 'A successful MQTT status response must cancel a pending retry');
    assert(ui.includes('_getMqttStatusFailureReason(err)'), 'MQTT reconnect failures should surface a readable status reason');
    assert(ui.includes("this._loadMqttStatus();") && ui.includes("currentBadge.textContent === '内存保护'"), 'MQTT disabled refresh should still query status and surface memory protection');

    // _handleMqttApiFail 应存在且在连续失败时显示可读原因。
    assert(ui.includes('_handleMqttApiFail'), '_handleMqttApiFail helper should exist');
    const handleFailFn = ui.match(/_handleMqttApiFail\(reason\)\s*\{[\s\S]{0,600}/);
    assert(handleFailFn, '_handleMqttApiFail function should exist');
    assert(handleFailFn[0].includes('_mqttApiFailCount'), '_handleMqttApiFail should check failure count');
    assert(handleFailFn[0].includes('_mqttApiFailCount < 2'), '_handleMqttApiFail should tolerate one transient failure');
    assert(handleFailFn[0].includes('_setMqttFailureStatus'), '_handleMqttApiFail should expose the failure reason after threshold');

    // 2025-06 修复: MQTT 状态轮询应有 setInterval 定期备份，确保状态不依赖纯 SSE
    const startPollingFn = ui.match(/_startMqttStatusPolling\(options\)\s*\{[\s\S]{0,1200}/);
    assert(startPollingFn, '_startMqttStatusPolling function definition should exist');
    assert(startPollingFn[0].includes('_mqttPollInterval'), '_startMqttStatusPolling should set _mqttPollInterval');
    assert(startPollingFn[0].includes('setInterval'), '_startMqttStatusPolling should use setInterval as backup poller');
    // 状态刷新入口应显式进入中性加载态，避免沿用旧的成功/失败状态。
    assert(ui.includes('_setMqttStatusLoading'), '_setMqttStatusLoading helper should exist');
    assert(ui.includes('mqtt-status-detecting'), '_setMqttStatusLoading should render the detecting class');

    // 2025-06 修复: _updateMqttStatusUI 应利用 lastError 区分"连接中"和"连接失败"
    // The status mapper also handles stopped/error states and actionable HTTP
    // diagnostics; keep the inspection window large enough to cover the full
    // function as those branches evolve.
    // Anchor on the object-property declaration. A nearby comment may also
    // mention `_updateMqttStatusUI`; matching the first occurrence made this
    // check silently inspect the wrong slice as the UI comments grew.
    const updateUIFn = balancedBlock(ui, '_updateMqttStatusUI: function');
    assert(updateUIFn, '_updateMqttStatusUI function should exist');
    assert(updateUIFn.includes('lastError'), '_updateMqttStatusUI should check lastError field');
    assert(updateUIFn.includes('连接失败'), '_updateMqttStatusUI should show 连接失败 when hasError');
    assert(updateUIFn.includes('stoppedByMemory'), '_updateMqttStatusUI should show temporary memory protection');
    assert(updateUIFn.includes('60000'), '_updateMqttStatusUI should use a 60-second connection timeout');
    assert(updateUIFn.includes('连接超时'), '_updateMqttStatusUI should show connection timeout');
    assert(ui.includes('statusReason'), 'MQTT UI should render backend statusReason/lastErrorMessage');
    // 不应再使用过于宽泛的 connecting 判断
    assert(!updateUIFn.includes('d.autoStartAttempted'), '_updateMqttStatusUI should NOT use overly broad autoStartAttempted in connecting check');

    // _stopMqttStatusPolling 应清除 _mqttPollInterval
    const stopPollingFn = balancedBlock(ui, '_stopMqttStatusPolling() {');
    assert(stopPollingFn, '_stopMqttStatusPolling function should exist');
    assert(stopPollingFn.includes('_mqttPollInterval'), '_stopMqttStatusPolling should clear _mqttPollInterval');
    assert(stopPollingFn.includes('_mqttStatusRequestSeq'), '_stopMqttStatusPolling must invalidate in-flight status callbacks');
}

function checkMqttStatusPollingFix() {
    const ui = read('web-src/modules/runtime/protocol/mqtt-config.js');
    const startPollingFn = ui.match(/_startMqttStatusPolling\(options\)\s*\{[\s\S]{0,1800}/);
    const startPollingBody = startPollingFn ? startPollingFn[0] : '';

    // ═══════════════════════════════════════════════════════════════════════
    // 规则1：禁止立即查询（防止资源耗尽）
    // _loadMqttStatus() 只能在 setTimeout 回调内部调用，不能在顶层执行
    // ═══════════════════════════════════════════════════════════════════════
    // 提取 _startMqttStatusPolling 中 _ensureBadgeNotStuck 之后、setTimeout 之前的区域
    const ensureBadgeIdx = startPollingBody.indexOf('_ensureBadgeNotStuck');
    const firstSetTimeoutIdx = startPollingBody.indexOf('setTimeout', ensureBadgeIdx > 0 ? ensureBadgeIdx : 0);
    assert(firstSetTimeoutIdx > 0, '[Rule1] _startMqttStatusPolling should contain setTimeout');
    // 在 setTimeout 之前的区域不应有 _loadMqttStatus 调用
    const preTimeoutRegion = startPollingBody.slice(0, firstSetTimeoutIdx);
    assert(!preTimeoutRegion.includes('_loadMqttStatus'), '[Rule1] 禁止在 setTimeout 外部直接调用 _loadMqttStatus — 会导致ESP32并发连接过多资源耗尽');
    // _loadMqttStatus 必须在 setTimeout 回调内部存在
    const postTimeoutRegion = startPollingBody.slice(firstSetTimeoutIdx);
    assert(postTimeoutRegion.includes('_loadMqttStatus'), '[Rule1] _loadMqttStatus 必须在 setTimeout 回调内部被调用');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则2：initialDelayMs 默认值必须 >= 1500
    // ═══════════════════════════════════════════════════════════════════════
    const delayDefaultMatch = startPollingBody.match(/options\.initialDelayMs\s*\|\|\s*(\d+)/);
    assert(delayDefaultMatch, '[Rule2] _startMqttStatusPolling 应包含 options.initialDelayMs || XXXX 默认值设置');
    if (delayDefaultMatch) {
        const defaultVal = parseInt(delayDefaultMatch[1], 10);
        assert(defaultVal >= 1500, '[Rule2] initialDelayMs 默认值必须 >= 1500ms（当前: ' + defaultVal + 'ms），防止过早请求与配置/SSE竞争连接池');
    }
    // protocol-lite-config.js 页面加载调用必须延迟 >= 3000ms（避免与页面chunk加载竞争TCP连接）
    // 支持两种模式：setTimeout(function(){ _loadMqttStatus() }, delay) 或 setTimeout(namedFn, delay)（递归轮询）
    const lite = read('web-src/modules/runtime/protocol/protocol-lite-config.js');
    const liteDelayMatch = lite.match(/setTimeout\s*\(\s*function\s*\(\)[\s\S]*?_loadMqttStatus[\s\S]*?\}\s*,\s*(\d+)\s*\)/)
        || lite.match(/setTimeout\s*\(\s*mqttPollOnce\s*,\s*(\d+)\s*\)/);
    assert(liteDelayMatch, '[Rule2] protocol-lite-config.js 应在 setTimeout 内调用 _loadMqttStatus 并指定延迟');
    if (liteDelayMatch) {
        const liteDelay = parseInt(liteDelayMatch[1], 10);
        assert(liteDelay >= 3000, '[Rule2] protocol-lite-config.js 的 setTimeout 延迟必须 >= 3000ms（当前: ' + liteDelay + 'ms），避免与页面chunk加载竞争TCP连接');
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 规则3：快速重试间隔必须 >= 3000ms
    // ═══════════════════════════════════════════════════════════════════════
    const loadStatusFn = ui.match(/_loadMqttStatus\(\)\s*\{[\s\S]{0,5000}/);
    const loadStatusBody = loadStatusFn ? loadStatusFn[0] : '';
    // 匹配 _mqttRetryTimer = setTimeout(..., XXXX) 中的间隔值
    const retryIntervalMatches = loadStatusBody.match(/_mqttRetryTimer\s*=\s*setTimeout\([\s\S]*?,\s*(\d+)\)/g) || [];
    assert(retryIntervalMatches.length > 0, '[Rule3] _loadMqttStatus 应包含 _mqttRetryTimer = setTimeout 重试设置');
    retryIntervalMatches.forEach(function(m) {
        const valMatch = m.match(/,\s*(\d+)\)$/);
        if (valMatch) {
            const interval = parseInt(valMatch[1], 10);
            assert(interval >= 3000, '[Rule3] 快速重试间隔必须 >= 3000ms（当前: ' + interval + 'ms），防止ESP32连接池耗尽');
        }
    });
    // 确认重试逻辑在 then 和 catch 两个分支中都存在
    const thenRetry = loadStatusBody.match(/\.then\([\s\S]*?_mqttRetryTimer\s*=\s*setTimeout/);
    const catchRetry = loadStatusBody.match(/\.catch\([\s\S]*?_mqttRetryTimer\s*=\s*setTimeout/);
    assert(thenRetry, '[Rule3] 快速重试应在 .then() 分支中存在（API返回失败时）');
    assert(catchRetry, '[Rule3] 快速重试应在 .catch() 分支中存在（网络错误时）');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则4：protocol-lite-config.js 必须包含状态查询（防止状态卡死）
    // ═══════════════════════════════════════════════════════════════════════
    assert(lite.includes('_loadMqttStatus'), '[Rule4] protocol-lite-config.js 必须调用 _loadMqttStatus — 否则MQTT badge会卡在"检测中..."');
    assert(lite.includes('mqtt.enabled !== false'), '[Rule4] protocol-lite-config.js 应对启用状态执行有限轮询');
    const disabledLiteBranch = balancedBlock(lite, 'else if (mqtt.enabled === false');
    assert(disabledLiteBranch, '[Rule4] protocol-lite-config.js 必须包含 MQTT 禁用状态分支');
    assert(disabledLiteBranch.includes('_loadMqttStatus'), '[Rule4] MQTT 禁用时必须读取一次后端状态，以展示内存保护原因');
    assert(disabledLiteBranch.includes('setTimeout'), '[Rule4] MQTT 禁用状态查询必须延迟执行，避免与页面资源争用连接');
    assert(!disabledLiteBranch.includes('mqtt-status-offline'), '[Rule4] MQTT 状态响应前不得由配置值提前显示离线');
    assert(!disabledLiteBranch.includes("textContent = '未连接'"), '[Rule4] MQTT 状态响应前必须保持中性检测态');
    assert(lite.includes('_setMqttStatusLoading'), '[Rule4] MQTT 配置加载后必须显式保持中性检测态');
    assert(lite.includes("typeof this._loadMqttStatus === 'function'"), '[Rule4] protocol-lite-config.js 应用 typeof 守卫防止模块未加载时报错');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则5：并发连接上限（前3秒内请求数 <= 3）
    // 时间线：0ms=配置请求+SSE(2个), >=1500ms=首次状态查询(第3个)
    // ═══════════════════════════════════════════════════════════════════════
    // 验证 _startMqttStatusPolling 不会在 delayMs 到期前发起 REST 请求
    assert(!preTimeoutRegion.includes('apiGet'), '[Rule5] setTimeout 之前不应有 apiGet 调用');
    assert(!preTimeoutRegion.includes('apiPost'), '[Rule5] setTimeout 之前不应有 apiPost 调用');
    assert(!preTimeoutRegion.includes('fetch('), '[Rule5] setTimeout 之前不应有 fetch 调用');
    // 纯 REST 轮询模式：不创建 SSE 持久连接，避免占用 TCP 槽位
    assert(!startPollingBody.includes('connectSSE'), '[Rule5] _startMqttStatusPolling 不应包含 connectSSE — 纯 REST 轮询模式，SSE 会占用 ESP32 TCP 槽位导致连接耗尽');
    // 确认默认延迟使得首次REST查询在 1500ms 之后
    if (delayDefaultMatch) {
        const defaultDelay = parseInt(delayDefaultMatch[1], 10);
        assert(defaultDelay >= 1500, '[Rule5] 默认延迟 >= 1500ms 确保前3个连接（config+SSE+首次状态）错开时间');
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 规则6：_stopMqttStatusPolling 必须清理所有timer
    // 必须清理：_mqttRetryTimer、_mqttStatusTimer、_mqttPollInterval
    // ═══════════════════════════════════════════════════════════════════════
    const stopPollingFn = balancedBlock(ui, '_stopMqttStatusPolling() {');
    const stopPollingBody = stopPollingFn || '';
    assert(stopPollingBody.includes('_mqttStatusTimer'), '[Rule6] _stopMqttStatusPolling 必须清理 _mqttStatusTimer');
    assert(stopPollingBody.includes('clearTimeout') && stopPollingBody.includes('_mqttStatusTimer'), '[Rule6] _stopMqttStatusPolling 必须 clearTimeout(_mqttStatusTimer)');
    assert(stopPollingBody.includes('_mqttPollInterval'), '[Rule6] _stopMqttStatusPolling 必须清理 _mqttPollInterval');
    assert(stopPollingBody.includes('clearInterval') && stopPollingBody.includes('_mqttPollInterval'), '[Rule6] _stopMqttStatusPolling 必须 clearInterval(_mqttPollInterval)');
    assert(stopPollingBody.includes('_mqttRetryTimer'), '[Rule6] _stopMqttStatusPolling 必须清理 _mqttRetryTimer');
    assert(stopPollingBody.match(/clearTimeout[\s\S]*?_mqttRetryTimer/), '[Rule6] _stopMqttStatusPolling 必须 clearTimeout(_mqttRetryTimer)');
    // 每个 timer 清理后必须置 null（防止悬垂引用）
    assert(stopPollingBody.includes('_mqttStatusTimer = null'), '[Rule6] _mqttStatusTimer 清理后必须置 null');
    assert(stopPollingBody.includes('_mqttPollInterval = null'), '[Rule6] _mqttPollInterval 清理后必须置 null');
    assert(stopPollingBody.includes('_mqttRetryTimer = null'), '[Rule6] _mqttRetryTimer 清理后必须置 null');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则7：刷新入口必须显式进入中性加载态（即时视觉反馈）
    // ═══════════════════════════════════════════════════════════════════════
    assert(ui.includes('_setMqttStatusLoading()'), '[Rule7] MQTT 状态刷新入口必须调用 _setMqttStatusLoading()');
    const loadingFn = ui.match(/_setMqttStatusLoading\(\)\s*\{[\s\S]{0,700}/);
    assert(loadingFn, '[Rule7] _setMqttStatusLoading 函数定义必须存在');
    assert(loadingFn && loadingFn[0].includes('mqtt-status-detecting'), '[Rule7] _setMqttStatusLoading 应显示检测中状态');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则8：protocol-lite-config.js 禁止创建持久SSE连接
    // 只允许一次性REST查询，不创建SSE长连接（ESP32 socket池有限）
    // ═══════════════════════════════════════════════════════════════════════
    assert(!lite.includes('connectSSE'), '[Rule8] protocol-lite-config.js 禁止包含 connectSSE — 不得创建持久SSE连接，会耗尽ESP32 lwIP socket池');
    assert(!lite.includes('_startMqttStatusPolling'), '[Rule8] protocol-lite-config.js 禁止包含 _startMqttStatusPolling — 只做一次性REST查询');
    assert(lite.includes('_loadMqttStatus'), '[Rule8] protocol-lite-config.js 应包含 _loadMqttStatus 的一次性调用');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则9：protocol-lite-config.js 中的 _loadMqttStatus 必须在 setTimeout 内
    // 不能直接调用，必须延迟执行避免与页面加载请求竞争socket
    // 支持两种模式：
    //   A) setTimeout(function(){ _loadMqttStatus() }, delay) — 直接内联
    //   B) function mqttPollOnce(){ _loadMqttStatus(); ... } setTimeout(mqttPollOnce, delay) — 递归轮询
    // ═══════════════════════════════════════════════════════════════════════
    const liteLoadStatusInTimeout = lite.match(/setTimeout\s*\(\s*function\s*\(\)\s*\{[^}]*_loadMqttStatus/)
        || lite.match(/function\s+mqttPollOnce\s*\(\)\s*\{[\s\S]*?_loadMqttStatus[\s\S]*?setTimeout\s*\(\s*mqttPollOnce\s*,\s*\d+/);
    assert(liteLoadStatusInTimeout, '[Rule9] protocol-lite-config.js 中 _loadMqttStatus 必须在 setTimeout 回调内调用（支持递归setTimeout轮询模式），不能直接调用');
    // 确认没有在 setTimeout 外部直接调用 _loadMqttStatus
    const liteLines = lite.split('\n');
    var liteDirectCallFound = false;
    for (var i = 0; i < liteLines.length; i++) {
        var line = liteLines[i].trim();
        // 跳过注释行
        if (line.startsWith('//') || line.startsWith('*')) continue;
        // 如果行包含 _loadMqttStatus 但不在 setTimeout 上下文内
        if (line.includes('_loadMqttStatus') && !line.includes('typeof') && !line.includes('//')) {
            // 检查前面几行是否有 setTimeout 或 mqttPollOnce 函数定义（递归轮询模式）
            var contextStart = Math.max(0, i - 10);
            var context = liteLines.slice(contextStart, i + 1).join('\n');
            if (!context.includes('setTimeout') && !context.includes('function mqttPollOnce')) {
                liteDirectCallFound = true;
                break;
            }
        }
    }
    assert(!liteDirectCallFound, '[Rule9] protocol-lite-config.js 中发现 _loadMqttStatus 在 setTimeout 外部直接调用');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则10：_startMqttStatusPolling 不应包含 SSE 连接
    // SSE 持久连接会占用 ESP32 TCP 槽位，纯 REST 轮询模式更安全
    // ═══════════════════════════════════════════════════════════════════════
    assert(!startPollingBody.includes('connectSSE'), '[Rule10] _startMqttStatusPolling 不应包含 connectSSE — 纯 REST 轮询模式，避免 SSE 占用 ESP32 TCP 槽位');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则11：ESP32 持久连接预算保护
    // protocol-lite-config.js 不允许 setInterval（会创建持续请求负载）
    // 所有网络调用必须是临时的（setTimeout + 单次调用）
    // ═══════════════════════════════════════════════════════════════════════
    assert(!lite.includes('setInterval'), '[Rule11] protocol-lite-config.js 禁止使用 setInterval — 所有网络调用必须是临时的（setTimeout + 单次调用），避免ESP32持久连接预算超限');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则12：_updateMqttStatusUI 必须有连接超时判断
    // 必须包含 _mqttConnectingStartTime、60秒超时、显示"连接超时"
    // 超时阈值必须 > lite路径轮询窗口(48s)，避免轮询期间提前触发超时
    // ═══════════════════════════════════════════════════════════════════════
    const updateUIFnR12 = balancedBlock(ui, '_updateMqttStatusUI: function');
    assert(updateUIFnR12, '[Rule12] _updateMqttStatusUI 函数应存在');
    if (updateUIFnR12) {
        const updateUIBody = updateUIFnR12;
        assert(updateUIBody.includes('_mqttConnectingStartTime'), '[Rule12] _updateMqttStatusUI 必须包含 _mqttConnectingStartTime 超时计时逻辑');
        assert(updateUIBody.includes('60000'), '[Rule12] _updateMqttStatusUI 必须有 60000（60秒）超时判断');
        assert(updateUIBody.includes('连接超时'), '[Rule12] _updateMqttStatusUI 超时后必须显示"连接超时"');
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 规则15：MQTT 页面加载后自动重连（解决 deferred 启动延迟问题）
    // ═══════════════════════════════════════════════════════════════════════
    assert(ui.includes('_mqttAutoReconnectTriggered'), 'MQTT UI should track auto-reconnect state to avoid duplicate triggers');
    assert(ui.includes('/api/mqtt/reconnect'), 'MQTT UI should auto-trigger /api/mqtt/reconnect when offline on first load');
    assert(ui.includes('apiPostSilent'), 'MQTT UI should use apiPostSilent for auto-reconnect to avoid UI disruption');
    assert(ui.includes('reconnectData.reconnecting === false'), 'MQTT UI should surface immediate reconnect failures returned in a 200 envelope');
    assert(ui.includes('_setMqttFailureStatus'), 'MQTT UI should expose reconnect failures in the status badge/reason panel');
    // 自动重连条件中禁止检查 connecting 状态（否则 autoStartStarted=true 会阻止触发）
    const reconnectBlock = ui.match(/_mqttAutoReconnectTriggered[\s\S]{0,400}api\/mqtt\/reconnect/);
    assert(reconnectBlock, 'auto-reconnect code block should exist between flag check and reconnect call');
    assert(!reconnectBlock[0].includes('!connecting'), 'CRITICAL: auto-reconnect MUST NOT check !connecting — autoStartStarted makes connecting=true, blocking reconnect forever');
    const liteCfg = read('web-src/modules/runtime/protocol/protocol-lite-config.js');
    const fullCfg = read('web-src/modules/runtime/protocol/protocol-config.js');
    assert(liteCfg.includes('_mqttAutoReconnectTriggered = false'), 'protocol-lite-config.js should reset auto-reconnect flag on page load');
    assert(fullCfg.includes('_mqttAutoReconnectTriggered = false'), 'protocol-config.js should reset auto-reconnect flag on page load');

    // ═══════════════════════════════════════════════════════════════════════
    // 规则13：MQTT lite路径轮询次数约束
    // mqttMaxPolls 必须为10，轮询间隔5000ms，总覆盖窗口 > 40s MQTT启动延迟
    // ═══════════════════════════════════════════════════════════════════════
    var maxPollsMatch = lite.match(/mqttMaxPolls\s*=\s*(\d+)/);
    assert(maxPollsMatch, '[Rule13] protocol-lite-config.js 必须包含 mqttMaxPolls 变量');
    if (maxPollsMatch) {
        var maxPolls = parseInt(maxPollsMatch[1], 10);
        assert(maxPolls === 10, '[Rule13] mqttMaxPolls 必须为 10（当前: ' + maxPolls + '）—— 覆盖40s+的MQTT连接窗口');
    }
    // 轮询间隔必须为 5000ms
    var pollIntervalMatch = lite.match(/setTimeout\s*\(\s*mqttPollOnce\s*,\s*(\d+)\s*\)/);
    assert(pollIntervalMatch, '[Rule13] protocol-lite-config.js 必须包含 setTimeout(mqttPollOnce, XXXX) 递归调用');
    if (pollIntervalMatch) {
        var pollInterval = parseInt(pollIntervalMatch[1], 10);
        assert(pollInterval === 5000, '[Rule13] 轮询间隔必须为 5000ms（当前: ' + pollInterval + 'ms）');
    }
    // 总轮询窗口必须覆盖 MQTT 40秒启动延迟
    // 最后一次轮询时间 = initialDelay + (maxPolls-1) * interval = 3000 + 9*5000 = 48000ms > 40000ms
    if (maxPollsMatch && pollIntervalMatch && liteDelayMatch) {
        var initialDelay = parseInt(liteDelayMatch[1], 10);
        assert(initialDelay >= 3000, '[Rule13] lite路径首次延迟必须 >= 3000ms（当前: ' + initialDelay + 'ms），避免与页面chunk加载竞争TCP连接');
        var lastPollTime = initialDelay + (parseInt(maxPollsMatch[1], 10) - 1) * parseInt(pollIntervalMatch[1], 10);
        assert(lastPollTime > 40000, '[Rule13] 总轮询窗口必须覆盖MQTT 40秒启动延迟（最后一次轮询时间: ' + lastPollTime + 'ms > 40000ms）');
    }

    // ═══════════════════════════════════════════════════════════════════════
    // 规则14：restartNetwork 非WiFi安全验证
    // 非WiFi分支中不得包含全局 disconnect()，必须使用 disconnectWiFi()，必须有AP恢复逻辑
    // ═══════════════════════════════════════════════════════════════════════
    var netMgr = read('src/network/NetworkManager.cpp');
    // 找到 restartNetwork 函数中非WiFi分支
    var nonWifiBranchMatch = netMgr.match(/\/\/ 对于非WiFi联网方式[\s\S]*?if \(wifiConfig\.networkType != NetworkType::NET_WIFI\)\s*\{([\s\S]*?)\n    \/\/ 对于WiFi STA模式/);
    assert(nonWifiBranchMatch, '[Rule14] NetworkManager.cpp 必须包含 restartNetwork 非WiFi分支');
    if (nonWifiBranchMatch) {
        var nonWifiBranch = nonWifiBranchMatch[1];
        // 非WiFi分支中不得包含全局 disconnect() 调用（this->disconnect() 或单独的 disconnect();)
        // 但允许 adapter->disconnect() 和 disconnectWiFi()
        var globalDisconnectRe = /(?<!->|adapter)disconnect\(\)/;
        var hasGlobalDisconnect = globalDisconnectRe.test(nonWifiBranch);
        assert(!hasGlobalDisconnect, '[Rule14] restartNetwork 非WiFi分支中不得调用全局 disconnect() —— 会关闭AP热点');

        // 必须包含 disconnectWiFi 调用（仅断 STA）
        assert(nonWifiBranch.includes('disconnectWiFi'), '[Rule14] restartNetwork 非WiFi分支必须包含 disconnectWiFi()（仅断STA）');

        // 必须包含 AP 恢复逻辑（startAPMode 或 WIFI_AP 检测）
        var hasAPRecovery = nonWifiBranch.includes('startAPMode') || nonWifiBranch.includes('WIFI_AP');
        assert(hasAPRecovery, '[Rule14] restartNetwork 非WiFi分支必须包含AP恢复逻辑（startAPMode 或 WIFI_AP 检测）');
    }
}

checkNtpLifecycle();
checkMqttLifecycle();
checkMqttStatusPollingFix();

if (failures.length > 0) {
    console.error(`\nMQTT/NTP lifecycle validation failed: ${failures.length} issue(s)`);
    process.exit(1);
}

console.log('MQTT/NTP lifecycle validation passed');
