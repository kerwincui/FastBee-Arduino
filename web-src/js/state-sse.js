// ============================================================
// AppState 子模块: SSE 连接管理
// 从 state.js 拆分，通过全局 AppState 对象注册
// 依赖: state.js (AppState 全局对象)
// ============================================================

(function() {
    'use strict';

    // SSE 连接状态属性（从 AppState 对象字面量中迁移）
    AppState._sseSource = null;
    AppState._sseHandlers = {};       // { eventType: [handler1, handler2, ...] }
    AppState._sseReconnectTimer = null;
    AppState._sseReconnectDelay = 1000;
    AppState._sseWatchdogTimer = null;
    AppState._sseLastEventAt = 0;
    AppState._sseRecoveryPending = false;

    AppState._sseNotifyRecovery = function(reason) {
        (this._sseHandlers.resync || []).slice().forEach(function(handler) {
            try { handler({ data: JSON.stringify({ reason: reason }) }); }
            catch (error) { console.warn('[SSE] recovery handler failed:', error); }
        });
    };

    AppState._sseRetry = function(source) {
        if (source && this._sseSource !== source) return;
        this.disconnectSSE();
        var handlers = this._sseHandlers || {};
        if (!Object.keys(handlers).some(function(key) { return handlers[key].length > 0; })) return;
        this._sseRecoveryPending = true;
        this._sseReconnectTimer = setTimeout(function() {
            this._sseReconnectTimer = null;
            this.connectSSE();
        }.bind(this), this._sseReconnectDelay);
        this._sseReconnectDelay = Math.min(this._sseReconnectDelay * 2, 30000);
    };

    AppState._sseWatch = function(source) {
        if (this._sseWatchdogTimer) clearTimeout(this._sseWatchdogTimer);
        this._sseWatchdogTimer = setTimeout(function() {
            this._sseWatchdogTimer = null;
            if (this._sseSource !== source) return;
            // Server heartbeat is 60s. Detect a half-open connection without
            // depending on EventSource eventually delivering an error.
            if (Date.now() - this._sseLastEventAt > 135000) {
                this._sseNotifyRecovery('stream_stalled');
                this._sseRetry(source);
            } else this._sseWatch(source);
        }.bind(this), 15000);
    };

    /**
     * 建立 SSE 连接
     * 如果已连接则跳过，支持指数退避重连
     */
    AppState.connectSSE = function() {
        if (this._sseSource) return;  // 已连接
        if (this._sseReconnectTimer) {
            clearTimeout(this._sseReconnectTimer);
            this._sseReconnectTimer = null;
        }

        try {
            this._sseSource = new EventSource('/api/events');
            var source = this._sseSource;
            this._sseLastEventAt = Date.now();
            ['heartbeat', 'sensor-data', 'modbus-data', 'resync'].forEach(function(type) {
                source.addEventListener(type, function() {
                    if (this._sseSource === source) this._sseLastEventAt = Date.now();
                }.bind(this));
            }.bind(this));
            this._sseWatch(source);
            // Only a completed connection resets backoff. Resetting on each
            // attempt turns persistent 503/401 responses into a retry storm.
            this._sseSource.onopen = function() {
                if (this._sseSource !== source) return;
                this._sseLastEventAt = Date.now();
                this._sseReconnectDelay = 1000;
                if (this._sseRecoveryPending) {
                    this._sseRecoveryPending = false;
                    this._sseNotifyRecovery('stream_reconnected');
                }
            }.bind(this);

            this._sseSource.onerror = function() {
                console.warn('[SSE] 连接错误，将自动重连');
                this._sseRetry(source);
            }.bind(this);

            // 绑定已注册的事件处理器
            this._rebindSSEHandlers();
        } catch (e) {
            console.error('[SSE] 创建 EventSource 失败:', e);
            this._sseRetry(this._sseSource);
        }
    };

    /**
     * 断开 SSE 连接并清理重连定时器
     */
    AppState.disconnectSSE = function() {
        if (this._sseWatchdogTimer) {
            clearTimeout(this._sseWatchdogTimer);
            this._sseWatchdogTimer = null;
        }
        if (this._sseSource) {
            this._sseSource.close();
            this._sseSource = null;
        }
        if (this._sseReconnectTimer) {
            clearTimeout(this._sseReconnectTimer);
            this._sseReconnectTimer = null;
        }
    };

    /**
     * 注册 SSE 事件处理器
     * @param {string} eventType - 事件类型
     * @param {function} handler - 事件处理函数
     */
    AppState.onSSEEvent = function(eventType, handler) {
        if (!this._sseHandlers[eventType]) {
            this._sseHandlers[eventType] = [];
        }
        this._sseHandlers[eventType].push(handler);
        // 如果已连接，立即绑定
        if (this._sseSource) {
            this._sseSource.addEventListener(eventType, handler);
        }
    };

    /**
     * 移除 SSE 事件处理器
     * @param {string} eventType - 事件类型
     * @param {function} handler - 事件处理函数
     */
    AppState.offSSEEvent = function(eventType, handler) {
        if (this._sseHandlers[eventType]) {
            var idx = this._sseHandlers[eventType].indexOf(handler);
            if (idx !== -1) this._sseHandlers[eventType].splice(idx, 1);
        }
        if (this._sseSource) {
            this._sseSource.removeEventListener(eventType, handler);
        }
    };

    /**
     * 重新绑定所有已注册的 SSE 事件处理器（重连后调用）
     * @private
     */
    AppState._rebindSSEHandlers = function() {
        var self = this;
        Object.keys(this._sseHandlers).forEach(function(eventType) {
            self._sseHandlers[eventType].forEach(function(handler) {
                self._sseSource.addEventListener(eventType, handler);
            });
        });
    };

})();
