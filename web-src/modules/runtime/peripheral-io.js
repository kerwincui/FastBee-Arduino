/**
 * On-demand peripheral diagnostics and safe device operations.
 */
(function() {
    AppState.registerModule('peripheral-io', {
        _periphIoModalBound: false,
        _periphIoSession: null,

        _bindPeripheralIoModalEvents() {
            if (this._periphIoModalBound) return;
            var closeIoModal = document.getElementById('close-peripheral-io-modal');
            if (closeIoModal) closeIoModal.addEventListener('click', () => this.closePeripheralIoModal());
            var cancelIoModal = document.getElementById('cancel-peripheral-io-btn');
            if (cancelIoModal) cancelIoModal.addEventListener('click', () => this.closePeripheralIoModal());
            var ioRefreshBtn = document.getElementById('peripheral-io-refresh-btn');
            if (ioRefreshBtn) ioRefreshBtn.addEventListener('click', () => this.refreshPeripheralIo(true));
            var uartReadBtn = document.getElementById('peripheral-uart-read-btn');
            if (uartReadBtn) uartReadBtn.addEventListener('click', () => this.refreshPeripheralIo(true));
            var uartSendBtn = document.getElementById('peripheral-uart-send-btn');
            if (uartSendBtn) uartSendBtn.addEventListener('click', () => this.sendPeripheralUart());
            var uartMode = document.getElementById('peripheral-uart-mode');
            if (uartMode) uartMode.addEventListener('change', () => this.updatePeripheralUartPayloadState());
            var uartPoll = document.getElementById('peripheral-uart-poll-interval');
            if (uartPoll) uartPoll.addEventListener('change', () => this._schedulePeripheralIoPolling());
            var uartPayload = document.getElementById('peripheral-uart-payload');
            if (uartPayload) uartPayload.addEventListener('input', () => this.updatePeripheralUartPayloadState());
            var uartClearBtn = document.getElementById('peripheral-uart-clear-btn');
            if (uartClearBtn) uartClearBtn.addEventListener('click', () => this.clearPeripheralUartOutput());
            var encoderResetBtn = document.getElementById('peripheral-encoder-reset-btn');
            if (encoderResetBtn) encoderResetBtn.addEventListener('click', () => this.resetPeripheralEncoder());
            var sdContent = document.getElementById('peripheral-sd-content');
            if (sdContent) sdContent.addEventListener('input', () => this._updateSdContentCount());
            [
                ['peripheral-sd-mount-btn', 'mount'],
                ['peripheral-sd-unmount-btn', 'unmount'],
                ['peripheral-sd-list-btn', 'list'],
                ['peripheral-sd-read-btn', 'read-file'],
                ['peripheral-sd-write-btn', 'write-file'],
                ['peripheral-sd-delete-btn', 'delete-file']
            ].forEach((entry) => {
                var button = document.getElementById(entry[0]);
                if (button) button.addEventListener('click', () => this.runPeripheralSdOperation(entry[1]));
            });
            [
                ['peripheral-rfid-scan-btn', 'scan'],
                ['peripheral-rfid-read-btn', 'read-block'],
                ['peripheral-rfid-write-btn', 'write-block']
            ].forEach((entry) => {
                var button = document.getElementById(entry[0]);
                if (button) button.addEventListener('click', () => this.runPeripheralRfidOperation(entry[1]));
            });
            [
                ['peripheral-i2c-scan-btn', 'scan'],
                ['peripheral-i2c-probe-btn', 'probe'],
                ['peripheral-i2c-read-btn', 'read'],
                ['peripheral-i2c-release-btn', 'release']
            ].forEach((entry) => {
                var button = document.getElementById(entry[0]);
                if (button) button.addEventListener('click', () => this.runPeripheralI2cOperation(entry[1]));
            });
            var dsNow = document.getElementById('peripheral-ds1302-now-btn');
            if (dsNow) dsNow.addEventListener('click', () => this.fillPeripheralDs1302BrowserTime());
            [
                ['peripheral-ds1302-set-btn', 'set-time'],
                ['peripheral-ds1302-start-btn', 'start'],
                ['peripheral-ds1302-stop-btn', 'stop']
            ].forEach((entry) => {
                var button = document.getElementById(entry[0]);
                if (button) button.addEventListener('click', () => this.runPeripheralDs1302Operation(entry[1]));
            });
            [
                ['peripheral-lcd1602-write-btn', 'display'],
                ['peripheral-lcd1602-clear-btn', 'clear'],
                ['peripheral-lcd1602-backlight-on-btn', 'backlight-on'],
                ['peripheral-lcd1602-backlight-off-btn', 'backlight-off']
            ].forEach((entry) => {
                var button = document.getElementById(entry[0]);
                if (button) button.addEventListener('click', () => this.runPeripheralLcd1602Operation(entry[1]));
            });
            this._periphIoModalBound = true;
        },

        async _ensurePeripheralIoModal() {
            var navSeq = this._pageNavSeq || 0;
            try {
                if (!PageLoader._modalTemplates?.['peripheral-io-modal']) {
                    await PageLoader.loadFragment('', 'peripheral-io');
                }
                if (navSeq !== (this._pageNavSeq || 0)) return null;
                var modal = this.ensureModal('peripheral-io-modal');
                if (!modal) throw new Error('Peripheral diagnostics template unavailable');
                this._periphIoModalBound = false;
                this._bindPeripheralIoModalEvents();
                return modal;
            } catch (error) {
                console.error('[PeripheralIO] Failed to load diagnostics:', error);
                if (typeof Notification !== 'undefined') {
                    Notification.error('外设诊断加载失败，请稍后重试', '外设诊断');
                }
                return null;
            }
        },


        _setPeripheralIoError(message) {
            var el = document.getElementById('peripheral-io-error');
            if (!el) return;
            el.textContent = message || '';
            el.classList.toggle('fb-hidden', !message);
        },

        _setPeripheralIoText(id, value) {
            var el = document.getElementById(id);
            if (el) el.textContent = (value === undefined || value === null || value === '') ? '--' : String(value);
        },

        _setPeripheralIoBusy(busy) {
            this._periphIoBusy = !!busy;
            const blocked = !!this._periphIoBlocked;
            [
                'peripheral-io-refresh-btn', 'peripheral-uart-read-btn', 'peripheral-uart-send-btn',
                'peripheral-encoder-reset-btn', 'peripheral-sd-mount-btn', 'peripheral-sd-unmount-btn',
                'peripheral-sd-list-btn', 'peripheral-sd-read-btn', 'peripheral-sd-write-btn',
                'peripheral-sd-delete-btn', 'peripheral-rfid-scan-btn', 'peripheral-rfid-read-btn',
                'peripheral-rfid-write-btn', 'peripheral-i2c-scan-btn', 'peripheral-i2c-probe-btn',
                'peripheral-i2c-read-btn', 'peripheral-i2c-release-btn', 'peripheral-ds1302-set-btn',
                'peripheral-ds1302-start-btn', 'peripheral-ds1302-stop-btn', 'peripheral-lcd1602-write-btn',
                'peripheral-lcd1602-clear-btn', 'peripheral-lcd1602-backlight-on-btn',
                'peripheral-lcd1602-backlight-off-btn'
            ]
                .forEach(function(id) {
                    var el = document.getElementById(id);
                    if (el) el.disabled = !!busy || blocked;
                });
            if (!busy && !this.isDeveloperModeEnabled()) {
                [
                    'peripheral-uart-send-btn', 'peripheral-encoder-reset-btn', 'peripheral-sd-mount-btn',
                    'peripheral-sd-unmount-btn', 'peripheral-sd-write-btn', 'peripheral-sd-delete-btn',
                    'peripheral-rfid-read-btn', 'peripheral-rfid-write-btn', 'peripheral-i2c-release-btn',
                    'peripheral-ds1302-set-btn', 'peripheral-ds1302-start-btn', 'peripheral-ds1302-stop-btn',
                    'peripheral-lcd1602-write-btn', 'peripheral-lcd1602-clear-btn',
                    'peripheral-lcd1602-backlight-on-btn', 'peripheral-lcd1602-backlight-off-btn'
                ].forEach((id) => {
                    var el = document.getElementById(id);
                    if (el) { el.disabled = true; el.title = this.getDeveloperModeDisabledText(); }
                });
            }
            if (this._periphIoType === 2) {
                ['peripheral-i2c-probe-btn', 'peripheral-i2c-read-btn', 'peripheral-i2c-release-btn', 'peripheral-i2c-channel'].forEach(function(id) {
                    var control = document.getElementById(id);
                    if (control) { control.disabled = true; control.title = '总线外设仅支持地址扫描，请在传感器外设中读取数据'; }
                });
            } else {
                var channel = document.getElementById('peripheral-i2c-channel');
                if (channel) { channel.disabled = false; channel.title = ''; }
            }
            if (!busy && this._periphIoType === 1) this.updatePeripheralUartPayloadState();
            if (!busy && this._periphIoKind === 'sd') this._updateSdContentCount();
        },

        async openPeripheralIoModal(id) {
            const session = {};
            this._periphIoSession = session;
            this._stopPeripheralIoPolling();
            this._periphI2cOperation = null;
            const modal = await this._ensurePeripheralIoModal();
            if (!modal || this._periphIoSession !== session) return;
            this._periphIoId = id;
            this._periphIoType = 0;
            this._periphIoKind = '';
            this._periphIoConfig = null;
            this._periphIoBlocked = '';
            this._setPeripheralIoError('');
            this._setPeripheralIoText('peripheral-io-name', '加载中...');
            this._setPeripheralIoText('peripheral-io-id', id);
            var panels = {
                uart: 'peripheral-uart-panel',
                encoder: 'peripheral-encoder-panel',
                sd: 'peripheral-sd-panel',
                rfid: 'peripheral-rfid-panel',
                'i2c-sensor': 'peripheral-i2c-sensor-panel',
                ds1302: 'peripheral-ds1302-panel',
                lcd1602: 'peripheral-lcd1602-panel'
            };
            Object.keys(panels).forEach(function(kind) {
                var panel = document.getElementById(panels[kind]);
                if (panel) panel.classList.add('fb-hidden');
            });
            this.showModal(modal);
            this._setPeripheralIoBusy(true);

            try {
                var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                var res = await getter('/api/peripherals/', { id: id });
                if (this._periphIoSession !== session) return;
                if (!res || !res.success || !res.data) throw new Error(res?.message || res?.error || '外设信息加载失败');
                var data = res.data;
                this._periphIoType = Number(data.type);
                this._periphIoKind = this._getPeripheralDiagnosticKind(data);
                this._periphIoConfig = data;
                if (!this._canOpenPeripheralDiagnostics(data)) {
                    this._periphIoBlocked = '当前固件未提供此类型的诊断能力';
                    throw new Error(this._periphIoBlocked);
                }
                this._periphIoBlocked = this._getPeripheralDiagnosticBlockReason(data);
                this._setPeripheralIoText('peripheral-io-name', data.name || data.typeName || id);
                this._setPeripheralIoText('peripheral-io-id', id);
                var activePanel = document.getElementById(panels[this._periphIoKind]);
                if (!activePanel) throw new Error('诊断面板资源尚未加载，请刷新页面重试');
                if (activePanel) activePanel.classList.remove('fb-hidden');
                if (this._periphIoKind === 'uart') {
                    this._setPeripheralIoText('peripheral-io-title', 'UART 终端');
                    this._setPeripheralIoText('peripheral-uart-framing', data.params?.framing || '--');
                    var payload = document.getElementById('peripheral-uart-payload');
                    if (payload) payload.value = '';
                    this.clearPeripheralUartOutput();
                    this.updatePeripheralUartPayloadState();
                } else if (this._periphIoKind === 'encoder') {
                    this._setPeripheralIoText('peripheral-io-title', '编码器读数');
                    this._setPeripheralIoText('peripheral-encoder-resolution-display', data.params?.resolution);
                } else if (this._periphIoKind === 'sd') {
                    this._setPeripheralIoText('peripheral-io-title', 'SD 卡文件');
                    this._setPeripheralIoText('peripheral-sd-output', '--');
                    this._updateSdContentCount();
                } else if (this._periphIoKind === 'rfid') {
                    this._setPeripheralIoText('peripheral-io-title', 'MFRC522 RFID');
                    this._setPeripheralIoText('peripheral-rfid-output', '--');
                } else if (this._periphIoKind === 'i2c-sensor') {
                    this._setPeripheralIoText('peripheral-io-title', 'I2C 探测与读取');
                    var driver = data.params?.driverName || data.params?.driver || data.driverName || data.driver || '--';
                    var address = data.params?.i2cAddress ?? data.params?.address ?? data.i2cAddress ?? data.address;
                    this._setPeripheralIoText('peripheral-i2c-driver', driver);
                    this._setPeripheralIoText('peripheral-i2c-address', this._formatI2cAddress(address));
                    this._populatePeripheralDiagnosticChannels(data, driver);
                    this._setPeripheralIoText('peripheral-i2c-value', '--');
                    this._setPeripheralIoText('peripheral-i2c-output', '--');
                } else if (this._periphIoKind === 'ds1302') {
                    this._setPeripheralIoText('peripheral-io-title', 'DS1302 实时时钟');
                    this.fillPeripheralDs1302BrowserTime();
                } else if (this._periphIoKind === 'lcd1602') {
                    this._setPeripheralIoText('peripheral-io-title', 'LCD1602 字符显示');
                    var cols = Number(data.params?.cols || 16);
                    ['peripheral-lcd1602-line1', 'peripheral-lcd1602-line2'].forEach(function(inputId) {
                        var input = document.getElementById(inputId);
                        if (input) input.maxLength = cols === 20 ? 20 : 16;
                    });
                } else {
                    throw new Error('该外设没有可用的诊断面板');
                }
                this._setPeripheralIoBusy(false);
                if (this._periphIoBlocked) {
                    this._setPeripheralIoError(this._periphIoBlocked);
                    return;
                }
                await this.refreshPeripheralIo(['encoder', 'ds1302', 'i2c-sensor'].indexOf(this._periphIoKind) >= 0);
                if (this._periphIoSession !== session) return;
                this._schedulePeripheralIoPolling();
            } catch (err) {
                if (this._periphIoSession !== session) return;
                this._setPeripheralIoBusy(false);
                this._setPeripheralIoError(err?.data?.message || err?.data?.error || err?.message || '外设信息加载失败');
            }
        },

        closePeripheralIoModal() {
            this._periphIoSession = null;
            this._stopPeripheralIoPolling();
            this._periphI2cOperation = null;
            var rfidKey = document.getElementById('peripheral-rfid-key');
            if (rfidKey) rfidKey.value = '';
            this._periphIoId = '';
            this._periphIoType = 0;
            this._periphIoKind = '';
            this._periphIoConfig = null;
            this._periphIoBlocked = '';
            this._periphIoBusy = false;
            this.hideModal('peripheral-io-modal');
        },

        _renderPeripheralIoReading(data) {
            if (this._periphIoKind === 'uart') {
                this._setPeripheralIoText('peripheral-uart-connected', data.connected === undefined ? '--' : (data.connected ? '已连接' : '未连接'));
                this._setPeripheralIoText('peripheral-uart-port', Number(data.port) >= 0 ? ('UART' + data.port) : '--');
                this._setPeripheralIoText('peripheral-uart-framing', data.framing);
                this._setPeripheralIoText('peripheral-uart-rx-available', data.rxAvailable !== undefined ? (data.rxAvailable + ' bytes') : '--');
                this._setPeripheralIoText('peripheral-uart-bytes-sent', data.bytesSent !== undefined ? (data.bytesSent + ' bytes') : '--');
                this._setPeripheralIoText('peripheral-uart-bytes-received', data.bytesReceived !== undefined ? (data.bytesReceived + ' bytes') : '--');
                if (data.length !== undefined) {
                    var length = Number(data.length) || 0;
                    if (length) {
                        this._periphUartTextLog = this._appendPeripheralIoLog(this._periphUartTextLog, data.text || '[非文本数据]');
                        this._periphUartHexLog = this._appendPeripheralIoLog(this._periphUartHexLog, data.hex || '');
                    }
                    this._setPeripheralIoText('peripheral-uart-text-output', this._periphUartTextLog || '[无新数据]');
                    this._setPeripheralIoText('peripheral-uart-hex-output', this._periphUartHexLog || '[无新数据]');
                }
            } else if (this._periphIoKind === 'encoder') {
                this._setPeripheralIoText('peripheral-encoder-count', data.count);
                this._setPeripheralIoText('peripheral-encoder-delta', data.delta);
                var direction = Number(data.direction);
                this._setPeripheralIoText('peripheral-encoder-direction', direction > 0 ? '正向 (+1)' : (direction < 0 ? '反向 (-1)' : '静止 (0)'));
                this._setPeripheralIoText('peripheral-encoder-frequency', data.frequencyHz !== undefined ? (Number(data.frequencyHz).toFixed(2) + ' Hz') : '--');
                this._setPeripheralIoText('peripheral-encoder-rpm', data.rpm !== undefined ? (Number(data.rpm).toFixed(2) + ' RPM') : '--');
            } else if (this._periphIoKind === 'sd') {
                this._renderPeripheralSdData(data);
            } else if (this._periphIoKind === 'rfid') {
                this._renderPeripheralRfidData(data);
            } else if (this._periphIoKind === 'i2c-sensor') {
                this._renderPeripheralI2cData(data);
            } else if (this._periphIoKind === 'ds1302') {
                this._renderPeripheralDs1302Data(data);
            } else if (this._periphIoKind === 'lcd1602') {
                this._renderPeripheralLcd1602Data(data);
            }
        },

        async refreshPeripheralIo(consumeData) {
            if (!this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            const session = this._periphIoSession;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var readData = this._periphIoType !== 2 && (this._periphIoKind === 'encoder' || consumeData === true);
                var path = readData ? '/api/peripherals/read' : '/api/peripherals/status';
                var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                var params = { id: this._periphIoId };
                if (readData && this._periphIoKind === 'sd') params.action = 'status';
                if (readData && this._periphIoKind === 'i2c-sensor') {
                    params.action = 'read';
                    params.channel = document.getElementById('peripheral-i2c-channel')?.value || '';
                }
                var res = await getter(path, params);
                if (this._periphIoSession !== session) return;
                if (!res || !res.success || !res.data) throw new Error(res?.message || res?.error || '读取失败');
                this._renderPeripheralIoReading(res.data);
            } catch (err) {
                if (this._periphIoSession !== session) return;
                if (this._periphIoKind === 'i2c-sensor') this._setPeripheralIoText('peripheral-i2c-value', '读取失败');
                this._setPeripheralIoError(err?.data?.message || err?.data?.error || err?.message || '读取失败');
            } finally {
                if (this._periphIoSession === session) this._setPeripheralIoBusy(false);
            }
        },

        _getPeripheralUartPayloadInfo() {
            var mode = document.getElementById('peripheral-uart-mode')?.value || 'text';
            var value = document.getElementById('peripheral-uart-payload')?.value || '';
            if (mode === 'hex') {
                var compact = value.replace(/\s+/g, '');
                var valid = compact.length > 0 && /^[0-9a-fA-F]+$/.test(compact) && compact.length % 2 === 0;
                return { mode: mode, value: value, bytes: Math.floor(compact.length / 2), valid: valid };
            }
            var bytes = (typeof TextEncoder === 'function') ? new TextEncoder().encode(value).length : unescape(encodeURIComponent(value)).length;
            return { mode: mode, value: value, bytes: bytes, valid: bytes > 0 };
        },

        updatePeripheralUartPayloadState() {
            var info = this._getPeripheralUartPayloadInfo();
            var count = document.getElementById('peripheral-uart-payload-count');
            var send = document.getElementById('peripheral-uart-send-btn');
            var payload = document.getElementById('peripheral-uart-payload');
            if (payload) payload.placeholder = info.mode === 'hex' ? '例如: 48 65 6C 6C 6F' : '输入要发送的文本';
            if (count) {
                count.textContent = info.bytes + ' / 128 bytes';
                count.classList.toggle('u-text-danger', !info.valid || info.bytes > 128);
            }
            if (send) send.disabled = this._periphIoBusy || !!this._periphIoBlocked || !this.isDeveloperModeEnabled() || !info.valid || info.bytes > 128;
        },

        async sendPeripheralUart() {
            if (this._periphIoType !== 1 || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            var info = this._getPeripheralUartPayloadInfo();
            if (!info.valid) {
                this._setPeripheralIoError(info.mode === 'hex' ? 'HEX 必须由完整字节对组成' : '发送内容不能为空');
                return;
            }
            if (info.bytes > 128) {
                this._setPeripheralIoError('单次发送不能超过 128 bytes');
                return;
            }
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var res = await apiPost('/api/peripherals/write', {
                    id: this._periphIoId,
                    data: info.value,
                    hex: info.mode === 'hex' ? 1 : 0
                });
                if (!res || !res.success) throw new Error(res?.message || res?.error || '发送失败');
                Notification.success(res.message || ('已发送 ' + info.bytes + ' bytes'), 'UART 终端');
                this._setPeripheralIoBusy(false);
                await this.refreshPeripheralIo(false);
            } catch (err) {
                this._setPeripheralIoError(err?.data?.message || err?.data?.error || err?.message || '发送失败');
                this._setPeripheralIoBusy(false);
            }
        },

        async resetPeripheralEncoder() {
            if (this._periphIoType !== 43 || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            if (!confirm('确定将编码器累计计数清零吗？')) return;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var res = await apiPost('/api/peripherals/write', { id: this._periphIoId, action: 'reset' });
                if (!res || !res.success) throw new Error(res?.message || res?.error || '清零失败');
                Notification.success('编码器计数已清零', '编码器');
                this._setPeripheralIoBusy(false);
                await this.refreshPeripheralIo(true);
            } catch (err) {
                this._setPeripheralIoError(err?.data?.message || err?.data?.error || err?.message || '清零失败');
                this._setPeripheralIoBusy(false);
            }
        },

        _stopPeripheralIoPolling() {
            if (this._periphIoPollTimer !== null) {
                clearInterval(this._periphIoPollTimer);
                this._periphIoPollTimer = null;
            }
        },

        _schedulePeripheralIoPolling() {
            this._stopPeripheralIoPolling();
            if (this._periphIoKind !== 'uart' || !this._periphIoId || this._periphIoBlocked) return;
            var interval = parseInt(document.getElementById('peripheral-uart-poll-interval')?.value || '0', 10);
            if (!Number.isFinite(interval) || interval < 250) return;
            this._periphIoPollTimer = setInterval(() => {
                if (this._periphIoId && this._periphIoKind === 'uart' && !this._periphIoBusy && !this._periphIoBlocked) {
                    this.refreshPeripheralIo(true);
                }
            }, interval);
        },

        _appendPeripheralIoLog(current, next) {
            var combined = current ? (current + '\n' + String(next || '')) : String(next || '');
            return combined.length > 8192 ? combined.slice(combined.length - 8192) : combined;
        },

        clearPeripheralUartOutput() {
            this._periphUartTextLog = '';
            this._periphUartHexLog = '';
            this._setPeripheralIoText('peripheral-uart-text-output', '[已清空]');
            this._setPeripheralIoText('peripheral-uart-hex-output', '[已清空]');
        },

        _setPeripheralIoOutput(id, value) {
            var output = document.getElementById(id);
            if (!output) return;
            if (value === undefined || value === null || value === '') output.textContent = '--';
            else if (typeof value === 'string') output.textContent = value;
            else output.textContent = JSON.stringify(value, null, 2);
        },

        _formatPeripheralBytes(value) {
            var bytes = Number(value);
            if (!Number.isFinite(bytes) || bytes < 0) return '--';
            var units = ['B', 'KiB', 'MiB', 'GiB'];
            var unit = 0;
            while (bytes >= 1024 && unit < units.length - 1) {
                bytes /= 1024;
                unit += 1;
            }
            return (unit === 0 ? Math.round(bytes) : bytes.toFixed(bytes >= 10 ? 1 : 2)) + ' ' + units[unit];
        },

        _updateSdContentCount() {
            var content = document.getElementById('peripheral-sd-content')?.value || '';
            var bytes = (typeof TextEncoder === 'function')
                ? new TextEncoder().encode(content).length : unescape(encodeURIComponent(content)).length;
            var count = document.getElementById('peripheral-sd-content-count');
            if (count) {
                count.textContent = bytes + ' / 4096 bytes';
                count.classList.toggle('u-text-danger', bytes > 4096);
            }
            var write = document.getElementById('peripheral-sd-write-btn');
            if (write) write.disabled = this._periphIoBusy || !!this._periphIoBlocked || !this.isDeveloperModeEnabled() || bytes > 4096;
            return bytes;
        },

        _renderPeripheralSdData(data) {
            this._setPeripheralIoText('peripheral-sd-mounted', data.mounted === undefined ? '--' : (data.mounted ? '已挂载' : '未挂载'));
            this._setPeripheralIoText('peripheral-sd-card-type', data.cardType || '--');
            this._setPeripheralIoText('peripheral-sd-total', this._formatPeripheralBytes(data.totalBytes ?? data.cardBytes));
            this._setPeripheralIoText('peripheral-sd-used', this._formatPeripheralBytes(data.usedBytes));
            var total = Number(data.totalBytes ?? data.cardBytes);
            var used = Number(data.usedBytes);
            var free = data.freeBytes !== undefined ? Number(data.freeBytes)
                : (Number.isFinite(total) && Number.isFinite(used) ? Math.max(0, total - used) : NaN);
            this._setPeripheralIoText('peripheral-sd-free', this._formatPeripheralBytes(free));
            this._setPeripheralIoText('peripheral-sd-filesystem', data.filesystem || data.fsType || '--');
            if (Array.isArray(data.entries)) {
                var lines = data.entries.map((entry) => {
                    var label = (entry.directory ? '[DIR] ' : '[FILE] ') + (entry.path || entry.name || '--');
                    return entry.directory ? label : (label + '  ' + this._formatPeripheralBytes(entry.size));
                });
                if (data.truncated) lines.push('[列表已截断]');
                this._setPeripheralIoOutput('peripheral-sd-output', lines.join('\n') || '[目录为空]');
            } else if (data.content !== undefined) {
                var content = document.getElementById('peripheral-sd-content');
                if (content) content.value = String(data.content);
                this._updateSdContentCount();
                this._setPeripheralIoOutput('peripheral-sd-output', '已读取 ' + (data.length ?? String(data.content).length) + ' bytes');
            }
        },

        async runPeripheralSdOperation(action) {
            if (this._periphIoKind !== 'sd' || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            var normalized = action === 'read-file' ? 'read' : action;
            var mutating = ['mount', 'unmount', 'write-file', 'delete-file'].indexOf(normalized) >= 0;
            if (mutating && !this.guardDeveloperModeAction()) return;
            var path = (document.getElementById('peripheral-sd-path')?.value || '/').trim();
            if (!path) path = '/';
            if (normalized === 'write-file' && this._updateSdContentCount() > 4096) {
                this._setPeripheralIoError('文件内容不能超过 4096 bytes');
                return;
            }
            if (normalized === 'delete-file' && !confirm('确定删除 SD 沙箱中的 ' + path + ' 吗？')) return;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var res;
                if (['status', 'list', 'read'].indexOf(normalized) >= 0) {
                    var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                    res = await getter('/api/peripherals/read', {
                        id: this._periphIoId,
                        action: normalized,
                        path: path,
                        maxBytes: 4096
                    });
                } else {
                    var content = document.getElementById('peripheral-sd-content')?.value || '';
                    res = await apiPost('/api/peripherals/write', {
                        id: this._periphIoId,
                        action: normalized,
                        path: path,
                        content: content,
                        append: 0,
                        confirm: normalized === 'delete-file' ? 'DELETE' : ''
                    });
                }
                if (!res || res.success === false) throw new Error(res?.message || res?.error || 'SD 操作失败');
                var data = res.data || res;
                this._renderPeripheralSdData(data);
                if (!Array.isArray(data.entries) && data.content === undefined) {
                    this._setPeripheralIoOutput('peripheral-sd-output', res.message || '操作成功');
                }
                if (mutating) Notification.success(res.message || 'SD 操作成功', 'SD 卡');
                this._setPeripheralIoBusy(false);
                if (normalized === 'mount' || normalized === 'unmount') await this.refreshPeripheralIo(false);
            } catch (err) {
                this._setPeripheralIoError(this._peripheralIoErrorMessage(err, 'SD 操作失败'));
                this._setPeripheralIoBusy(false);
            }
        },

        _normalizePeripheralHex(value) {
            return String(value || '').replace(/[\s:-]+/g, '').toUpperCase();
        },

        _renderPeripheralRfidData(data) {
            this._setPeripheralIoText('peripheral-rfid-ready', data.initialized === undefined
                ? (data.cardPresent === undefined ? '--' : '已初始化')
                : (data.initialized ? '已初始化' : '未初始化'));
            this._setPeripheralIoText('peripheral-rfid-uid', data.uid || '--');
            this._setPeripheralIoText('peripheral-rfid-card-type', data.cardType || '--');
            var blockData = data.hex || data.blockData || data.content;
            if (blockData !== undefined) {
                var input = document.getElementById('peripheral-rfid-data');
                if (input) input.value = String(blockData);
                this._setPeripheralIoOutput('peripheral-rfid-output', String(blockData));
            }
        },

        async runPeripheralRfidOperation(action) {
            if (this._periphIoKind !== 'rfid' || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            var needsKey = action === 'read-block' || action === 'write-block';
            var block = parseInt(document.getElementById('peripheral-rfid-block')?.value || '', 10);
            var key = this._normalizePeripheralHex(document.getElementById('peripheral-rfid-key')?.value);
            var blockData = this._normalizePeripheralHex(document.getElementById('peripheral-rfid-data')?.value);
            if (needsKey && (!Number.isInteger(block) || block < 1 || block > 62 || ((block + 1) % 4) === 0)) {
                this._setPeripheralIoError('请选择 1-62 之间的普通数据块，扇区尾块不可写入或读取');
                return;
            }
            if (needsKey && !/^[0-9A-F]{12}$/.test(key)) {
                this._setPeripheralIoError('Key A 必须是 12 位 HEX');
                return;
            }
            if (action === 'write-block' && !/^[0-9A-F]{32}$/.test(blockData)) {
                this._setPeripheralIoError('块数据必须是 16 bytes（32 位 HEX）');
                return;
            }
            if (needsKey && !this.guardDeveloperModeAction()) return;
            if (action === 'write-block' && !confirm('确定写入 RFID 数据块 ' + block + ' 并执行写后校验吗？')) return;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var res;
                if (action === 'scan') {
                    var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                    res = await getter('/api/peripherals/read', { id: this._periphIoId, action: 'scan' });
                } else {
                    res = await apiPost('/api/peripherals/write', {
                        id: this._periphIoId,
                        action: action,
                        block: block,
                        key: key,
                        data: action === 'write-block' ? blockData : '',
                        confirm: action === 'write-block' ? 'WRITE_BLOCK' : ''
                    });
                }
                if (!res || res.success === false) throw new Error(res?.message || res?.error || 'RFID 操作失败');
                var data = res.data || res;
                this._renderPeripheralRfidData(data);
                this._setPeripheralIoOutput('peripheral-rfid-output', data.hex || data.blockData || res.message || '操作成功');
                if (action === 'write-block') Notification.success('RFID 数据块写入并校验成功', 'RFID');
            } catch (err) {
                this._setPeripheralIoError(this._peripheralIoErrorMessage(err, 'RFID 操作失败'));
            } finally {
                if (needsKey) {
                    var keyInput = document.getElementById('peripheral-rfid-key');
                    if (keyInput) keyInput.value = '';
                }
                this._setPeripheralIoBusy(false);
            }
        },

        _populatePeripheralDiagnosticChannels(config, driver) {
            var select = document.getElementById('peripheral-i2c-channel');
            if (!select) return;
            var meta = typeof this._getPeripheralDriverMeta === 'function' ? this._getPeripheralDriverMeta(38, driver) : null;
            var mode = Number(config.params?.mode || 0);
            var channels = (meta?.channels || []).filter(function(channel) {
                return !channel.mode || Number(channel.mode) === mode + 1;
            });
            select.innerHTML = '';
            channels.forEach(function(channel) {
                var option = document.createElement('option');
                option.value = channel.name;
                option.textContent = (channel.label || channel.name) + (channel.unit ? ' (' + channel.unit + ')' : '');
                select.appendChild(option);
            });
            if (!channels.length) select.innerHTML = '<option value="">默认通道</option>';
            var saved = config.params?.channel || config.channel || '';
            if (channels.some(function(channel) { return channel.name === saved; })) select.value = saved;
        },

        _renderPeripheralI2cData(data) {
            this._setPeripheralIoText('peripheral-i2c-driver', data.driverName || data.driver || this._periphIoConfig?.params?.driver || '--');
            this._setPeripheralIoText('peripheral-i2c-address', this._formatI2cAddress(data.i2cAddress ?? data.address ?? this._periphIoConfig?.params?.address));
            if (data.valid === false || data.success === false || data.error ||
                (data.value !== undefined && (data.value === null || !Number.isFinite(Number(data.value))))) {
                this._setPeripheralIoText('peripheral-i2c-value', '读数无效');
                this._setPeripheralIoOutput('peripheral-i2c-output', data);
                return;
            }
            if (data.value !== undefined) {
                this._setPeripheralIoText('peripheral-i2c-value', String(data.value) + (data.unit ? ' ' + data.unit : ''));
            }
            if (Array.isArray(data.addresses)) {
                var scanText = data.addresses.map((address) => this._formatI2cAddress(address)).join(', ') ||
                    (data.truncated ? '[已扫描范围内未发现设备]' : '[未发现设备]');
                scanText += data.truncated ? '\n[扫描未完成，仅显示已扫描结果]' : '\n[扫描完成]';
                if (data.truncated && Number.isInteger(data.nextAddress)) {
                    scanText += ' 下一个地址：' + this._formatI2cAddress(data.nextAddress);
                }
                this._setPeripheralIoOutput('peripheral-i2c-output', scanText);
            } else if (data.value !== undefined) {
                this._setPeripheralIoOutput('peripheral-i2c-output', {
                    channel: data.channel || document.getElementById('peripheral-i2c-channel')?.value || 'default',
                    value: data.value,
                    unit: data.unit || '',
                    timestamp: data.sampledAt ?? data.timestamp ?? null
                });
            }
        },

        async runPeripheralI2cOperation(action) {
            if (this._periphIoKind !== 'i2c-sensor' || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            if (this._periphIoType === 2 && action !== 'scan') return;
            if (action === 'release' && !this.guardDeveloperModeAction()) return;
            const session = this._periphIoSession;
            var operation = {};
            this._periphI2cOperation = operation;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var payload = {
                    id: this._periphIoId,
                    action: action,
                    channel: document.getElementById('peripheral-i2c-channel')?.value || ''
                };
                var res;
                if (action === 'release') res = await apiPost('/api/peripherals/write', payload);
                else {
                    var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                    if (action === 'scan') {
                        payload.startAddress = 1;
                        var addresses = [];
                        this._setPeripheralIoOutput('peripheral-i2c-output', '[扫描未完成，正在扫描总线…]');
                        for (var round = 0; round < 126; round++) {
                            if (this._periphIoSession !== session || this._periphI2cOperation !== operation) return;
                            res = await getter('/api/peripherals/read', payload);
                            if (this._periphIoSession !== session || this._periphI2cOperation !== operation) return;
                            if (!res || res.success === false) throw new Error(res?.message || res?.error || 'I2C 扫描失败');
                            var scan = res.data || res;
                            if (!Array.isArray(scan.addresses)) throw new Error('I2C 扫描结果无效');
                            scan.addresses.forEach(function(address) {
                                if (Number.isInteger(address) && address >= 1 && address <= 126 && addresses.indexOf(address) < 0) addresses.push(address);
                            });
                            addresses.sort(function(a, b) { return a - b; });
                            this._renderPeripheralI2cData(Object.assign({}, scan, { addresses: addresses, count: addresses.length }));
                            if (!scan.truncated) return;
                            if (!Number.isInteger(scan.nextAddress) || scan.nextAddress <= payload.startAddress || scan.nextAddress > 126) {
                                throw new Error('扫描未完成：总线繁忙或扫描没有进展，请稍后重试');
                            }
                            payload.startAddress = scan.nextAddress;
                            await new Promise(function(resolve) { setTimeout(resolve, 25); });
                        }
                        throw new Error('扫描未完成，请稍后重试');
                    }
                    res = await getter('/api/peripherals/read', payload);
                }
                if (this._periphIoSession !== session || this._periphI2cOperation !== operation) return;
                if (!res || res.success === false) throw new Error(res?.message || res?.error || 'I2C 操作失败');
                var data = res.data || res;
                this._renderPeripheralI2cData(data);
                if (data.value === undefined && !Array.isArray(data.addresses)) {
                    this._setPeripheralIoOutput('peripheral-i2c-output', res.message || data);
                }
            } catch (err) {
                if (this._periphIoSession === session && this._periphI2cOperation === operation) {
                    if (action !== 'scan') this._setPeripheralIoText('peripheral-i2c-value', '读取失败');
                    this._setPeripheralIoError(this._peripheralIoErrorMessage(err, 'I2C 操作失败'));
                }
            } finally {
                if (this._periphIoSession === session && this._periphI2cOperation === operation) {
                    this._periphI2cOperation = null;
                    this._setPeripheralIoBusy(false);
                }
            }
        },

        fillPeripheralDs1302BrowserTime() {
            var input = document.getElementById('peripheral-ds1302-datetime');
            if (!input) return;
            var now = new Date();
            var pad = function(value) { return String(value).padStart(2, '0'); };
            input.value = now.getFullYear() + '-' + pad(now.getMonth() + 1) + '-' + pad(now.getDate()) +
                'T' + pad(now.getHours()) + ':' + pad(now.getMinutes()) + ':' + pad(now.getSeconds());
        },

        _renderPeripheralDs1302Data(data) {
            this._setPeripheralIoText('peripheral-ds1302-ready', data.initialized === undefined ? '--' : (data.initialized ? '已初始化' : '未初始化'));
            this._setPeripheralIoText('peripheral-ds1302-running', data.running === undefined ? '--' : (data.running ? '运行中' : '已暂停'));
            var formatted = data.formatted;
            if (!formatted && data.year !== undefined) {
                var pad = function(value) { return String(value).padStart(2, '0'); };
                formatted = data.year + '-' + pad(data.month) + '-' + pad(data.day) + ' ' +
                    pad(data.hour) + ':' + pad(data.minute) + ':' + pad(data.second);
            }
            this._setPeripheralIoText('peripheral-ds1302-current', formatted || '--');
            this._setPeripheralIoOutput('peripheral-ds1302-output', formatted || data);
        },

        async runPeripheralDs1302Operation(action) {
            if (this._periphIoKind !== 'ds1302' || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            if (!this.guardDeveloperModeAction()) return;
            var payload = { id: this._periphIoId, action: action };
            if (action === 'set-time') {
                var value = document.getElementById('peripheral-ds1302-datetime')?.value || '';
                var date = value ? new Date(value) : null;
                if (!date || Number.isNaN(date.getTime())) {
                    this._setPeripheralIoError('请选择有效的日期时间');
                    return;
                }
                payload.year = date.getFullYear();
                payload.month = date.getMonth() + 1;
                payload.day = date.getDate();
                payload.hour = date.getHours();
                payload.minute = date.getMinutes();
                payload.second = date.getSeconds();
                payload.dayOfWeek = ((date.getDay() + 6) % 7) + 1;
                payload.action = 'set-datetime';
            } else if (action === 'start' || action === 'stop') {
                payload.action = 'set-running';
                payload.running = action === 'start' ? 1 : 0;
            }
            if (action === 'stop' && !confirm('确定暂停 DS1302 计时吗？')) return;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var res = await apiPost('/api/peripherals/write', payload);
                if (!res || res.success === false) throw new Error(res?.message || res?.error || 'DS1302 操作失败');
                this._setPeripheralIoOutput('peripheral-ds1302-output', res.message || '操作成功');
                this._setPeripheralIoBusy(false);
                await this.refreshPeripheralIo(true);
            } catch (err) {
                this._setPeripheralIoError(this._peripheralIoErrorMessage(err, 'DS1302 操作失败'));
                this._setPeripheralIoBusy(false);
            }
        },

        _renderPeripheralLcd1602Data(data) {
            this._setPeripheralIoText('peripheral-lcd1602-ready', data.initialized === undefined ? '--' : (data.initialized ? '已初始化' : '未初始化'));
            this._setPeripheralIoText('peripheral-lcd1602-size', data.cols && data.rows ? (data.cols + ' x ' + data.rows) : '--');
            this._setPeripheralIoText('peripheral-lcd1602-backlight-state', data.backlight === undefined ? '--' : (data.backlight ? '开启' : '关闭'));
        },

        async runPeripheralLcd1602Operation(action) {
            if (this._periphIoKind !== 'lcd1602' || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            if (!this.guardDeveloperModeAction()) return;
            var payload = { id: this._periphIoId, action: action };
            var printRows = [];
            if (action === 'display') {
                var line1 = document.getElementById('peripheral-lcd1602-line1')?.value || '';
                var line2 = document.getElementById('peripheral-lcd1602-line2')?.value || '';
                if (!/^[\x20-\x7E]*$/.test(line1) || !/^[\x20-\x7E]*$/.test(line2)) {
                    this._setPeripheralIoError('LCD1602 文本仅支持可打印 ASCII 字符');
                    return;
                }
                if (!line1 && !line2) {
                    this._setPeripheralIoError('请输入至少一行要显示的文本');
                    return;
                }
                if (line1) printRows.push({ row: 0, text: line1 });
                if (line2) printRows.push({ row: 1, text: line2 });
            } else if (action === 'backlight-on' || action === 'backlight-off') {
                payload.action = 'backlight';
                payload.enabled = action === 'backlight-on' ? 1 : 0;
            }
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                var res;
                if (action === 'display') {
                    for (var i = 0; i < printRows.length; i += 1) {
                        res = await apiPost('/api/peripherals/write', {
                            id: this._periphIoId,
                            action: 'print',
                            text: printRows[i].text,
                            row: printRows[i].row,
                            col: 0
                        });
                        if (!res || res.success === false) break;
                    }
                } else {
                    res = await apiPost('/api/peripherals/write', payload);
                }
                if (!res || res.success === false) throw new Error(res?.message || res?.error || 'LCD1602 操作失败');
                this._setPeripheralIoOutput('peripheral-lcd1602-output', res.message || '操作成功');
                Notification.success(res.message || 'LCD1602 操作成功', 'LCD1602');
                this._setPeripheralIoBusy(false);
                await this.refreshPeripheralIo(false);
            } catch (err) {
                this._setPeripheralIoError(this._peripheralIoErrorMessage(err, 'LCD1602 操作失败'));
                this._setPeripheralIoBusy(false);
            }
        },

    });
})();
