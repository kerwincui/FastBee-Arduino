/**
 * On-demand peripheral diagnostics and safe device operations.
 */
(function() {
    AppState.registerModule('peripheral-io', {
        _periphIoModalBound: false,
        _periphIoSession: null,

        closePeripheralResources() {
            this._periphResourceRequest = null;
            const dialog = this._periphResourceDialog;
            this._periphResourceDialog = null;
            if (!dialog) return;
            if (typeof this.hideModal === 'function') this.hideModal(dialog);
            else dialog.remove();
        },

        openPeripheralResources() {
            this.closePeripheralResources();
            const overlay = document.createElement('div');
            overlay.className = 'modal';
            overlay.id = 'peripheral-resources-modal';
            const content = document.createElement('div');
            content.className = 'modal-content';
            const header = document.createElement('div');
            header.className = 'modal-header';
            const title = document.createElement('h2');
            title.className = 'modal-title';
            title.textContent = '配置与运行资源';
            const closeBtn = document.createElement('button');
            closeBtn.className = 'modal-close-btn';
            closeBtn.textContent = '×';
            closeBtn.setAttribute('aria-label', '关闭');
            header.append(title, closeBtn);
            const body = document.createElement('div');
            body.className = 'modal-body';
            const note = document.createElement('p');
            note.className = 'field-hint';
            note.textContent = '运行记录仅覆盖部分受管租约；未探测硬件，不能据此认定引脚空闲。点击刷新读取一次。';
            const result = document.createElement('pre');
            result.style.cssText = 'white-space:pre-wrap;overflow-wrap:anywhere;max-height:55vh;overflow:auto;margin:0;font-size:12px;line-height:1.6';
            result.setAttribute('role', 'status');
            body.append(note, result);
            const footer = document.createElement('div');
            footer.className = 'modal-footer';
            const refresh = document.createElement('button');
            refresh.type = 'button';
            refresh.className = 'fb-btn fb-btn-primary';
            refresh.textContent = '刷新快照';
            refresh.addEventListener('click', () => this.refreshPeripheralResources());
            footer.appendChild(refresh);
            content.append(header, body, footer);
            overlay.appendChild(content);
            document.body.appendChild(overlay);
            this._periphResourceDialog = overlay;
            this.showModal(overlay);
            this.refreshPeripheralResources();
        },
        async refreshPeripheralResources() {
            const dialog = this._periphResourceDialog;
            if (!dialog) return;
            const token = this._periphResourceRequest = {};
            const page = this.currentPage;
            const result = dialog.querySelector('pre');
            result.textContent = '读取中；旧快照已失效';
            try {
                const response = await apiGetFresh('/api/peripherals/resources');
                if (this._periphResourceRequest !== token || this.currentPage !== page) return;
                if (!response?.success) throw new Error();
                result.textContent = this._peripheralResourcesText(response.data);
            } catch (error) {
                if (this._periphResourceRequest === token && this.currentPage === page)
                    result.textContent = '快照不可用；运行占用未知，请稍后手动刷新';
            }
        },

        _peripheralResourcesText(data) {
            const runtime = data?.runtime;
            if (data?.claimScope !== 'configuration' || runtime?.schemaVersion !== 1 ||
                runtime.scope !== 'managed_leases' || runtime.complete !== false || runtime.hardwareProbed !== false)
                throw new Error('资源快照契约不完整');
            const lines = ['配置板型：' + data.profileId + '；实际运行板型：' + runtime.activeProfileId,
                '配置统计（启用 / 全部 / 规划容量；不代表实际空闲）：'];
            const number = (value) => Number.isSafeInteger(value) && value >= 0 ? value : '未知';
            if (data.configurationStatus === 'observed' && Array.isArray(data.resources)) {
                data.resources.forEach((row) => lines.push(row.label + '：' + number(row.used) + ' / ' +
                    number(row.configured) + ' / ' + number(row.capacity)));
                if (data.claimsTruncated || data.issuesTruncated) lines.push('配置统计有截断，不完整');
            } else lines.push('配置读取忙或未知');
            lines.push('\n运行租约（各项独立快照）：');
            for (const [key, name, capacity] of [['i2c', 'I2C', 16], ['spi', 'SPI', 16], ['uart', 'UART', 2],
                ['controlOutput', '控制输出', 4], ['encoder', '编码器ISR槽', 4], ['dhtRmt', 'DHT RMT', 1],
                ['ledc', '外设/脚本LEDC', 16], ['backlightLedc', '屏幕背光LEDC', 1],
                ['adc', '受管ADC', 32], ['gpio', '受管GPIO', 49], ['dac', '原生DAC', 2],
                ['neopixel', 'NeoPixel对象', 1], ['stepper', '步进四相输出', 16]]) {
                const source = runtime[key];
                const observed = source?.status === 'observed' && Array.isArray(source.leases) &&
                    source.leases.length <= capacity &&
                    source.leaseCount === source.leases.length +
                        (key === 'spi' && source.network?.reserved && source.network.dedicatedHost ? 1 : 0);
                lines.push(name + '：' + (source?.status === 'unsupported' ? '当前构建未支持' :
                    observed ? number(source.leaseCount) + ' 项保留' : '忙或未知'));
                if (!observed) continue;
                if (source.truncated) lines.push('  列表已截断，不完整');
                if (key === 'backlightLedc') lines.push('  屏幕任务发布于 ' + number(source.sampleAgeMs) + 'ms 前');
                if (key === 'adc') {
                    const completeUnits = Array.isArray(source.units) && source.units.length <= 2 && source.unitCount === source.units.length;
                    lines.push('  ADC单元记录：' + (completeUnits ? number(source.unitCount) : '未知') +
                        '；校准句柄：' + number(source.calibrationCount) + '；待清理：' + number(source.calibrationCleanupPending));
                    if (completeUnits) source.units.forEach((unit) => lines.push('  单元' + number(unit.unit) +
                        (unit.cleanupFailed ? '：释放失败，仍保留' : unit.retained ? '：保留' : '：未保留')));
                    if (Array.isArray(source.channels) && source.channels.length <= 20 && source.channelCount === source.channels.length)
                        source.channels.forEach((channel) => lines.push('  通道记录 ADC' + number(channel.unit) +
                            '/CH' + number(channel.channel) + ' · GPIO' + number(channel.pin) +
                            (channel.configurationKnown ? ' · 配置已确认' : ' · 配置未知')));
                    else lines.push('  ADC通道记录未知');
                }
                if (source.retainedWithoutClients) lines.push('  总线仍保留，无受管客户端');
                if (source.busState === 'retained_uninitialized') lines.push('  初始化未确认，租约仍保留');
                if (key === 'i2c') lines.push('  SDA/SCL ' + source.sda + '/' + source.scl);
                if (key === 'spi' && source.network?.reserved) lines.push('  网络预留：' + source.network.pins.join('/') +
                    (source.network.dedicatedHost ? '（独立主机）' : '（已计入客户端）'));
                source.leases.forEach((lease) => {
                    const owner = lease.ownerKnown ? lease.ownerId : '归属未解析';
                    const target = (lease.resourceId ? lease.resourceId + ' ← ' : '') + owner +
                        (lease.ownerRole && lease.ownerRole !== 'unknown' ? ' (' + lease.ownerRole + ')' : '');
                    const location = key === 'uart' ? 'UART' + lease.port : key === 'spi' ? 'CS' + lease.cs :
                        key === 'ledc' || key === 'backlightLedc' ? 'GPIO' + lease.pin + '/CH' + number(lease.channel) :
                        key === 'adc' ? 'GPIO' + lease.pin + '/ADC' + number(lease.unit) + '/CH' + number(lease.channel) :
                        ['controlOutput', 'dhtRmt', 'gpio', 'dac', 'neopixel'].includes(key) ? 'GPIO' + number(lease.pin) :
                        key === 'stepper' ? 'IN1–4 ' + (Array.isArray(lease.pins) && lease.pins.length === 4 ? lease.pins.map(number).join('/') : '未知') : key === 'encoder' ?
                            'A/B ' + lease.pinA + '/' + lease.pinB : lease.addressReserved ? '地址' + lease.address : '总线';
                    lines.push('  ' + target + ' · ' + location + ' · ' +
                        (lease.cleanupFailed ? '退役清理失败' : lease.cleanupPending ? '待退役' : lease.fault ? '故障保留' : '已保留'));
                    if (key === 'gpio') lines.push('    模式 ' + (lease.mode || '未知') +
                        '；中断登记 ' + (lease.interruptCleanupPending ? '清理待确认' :
                            lease.interruptAttached === true ? '已附加' : lease.interruptAttached === false ? '未附加' : '未知'));
                    if (key === 'encoder') lines.push('    计数接收 ' + (lease.active === true ? '开启' : lease.active === false ? '停止' : '未知') +
                        '；中断 ' + (lease.interruptCleanupPending ? '清理待确认' : lease.interruptAttached === true ? 'SDK已确认' : lease.interruptAttached === false ? '未附加' : '未知'));
                    if (key === 'neopixel') lines.push('    配置灯珠 ' + number(lease.configuredLedCount) +
                        '；发送结果 ' + (lease.writeAttempted === true ? number(lease.lastWriteResult) : '未尝试') +
                        '；释放结果 ' + (lease.releaseAttempted === true ? number(lease.lastReleaseResult) : '未尝试'));
                    if (key === 'stepper') lines.push('    相位输出 ' + (lease.running === true ? '调度中' : lease.running === false ? '停止' : '未知') +
                        '；保留掩码 ' + number(lease.retainedMask) + '；GPIO待清理掩码 ' + number(lease.gpioCleanupPendingMask));
                    if (key === 'adc') lines.push('    返回位数 ' + number(lease.resolutionBits) +
                        (lease.transient ? '；批次临时租约' : '；持久租约'));
                });
                if (source.cleanupPending === null) lines.push('  退役阶段未知');
            }
            lines.push('\n其它LEDC/RMT/ADC/GPIO及未受管使用：未知。ADC零消费者不代表单元已释放；控制输出、编码器和步进与GPIO记录不可重复计数。LEDC自动通道未知；编码器仅ISR槽；NeoPixel仅对象与引脚保留，不是RMT通道数或点亮证明；步进掩码按IN1–4，调度不代表实测位置。DHT仅接收资源。SDMMC不支持，SDSPI列入SPI。');
            return lines.join('\n');
        },

        async _selectMcpGroupChannelsLoaded(id, getter) {
            const response = await getter('/api/peripherals/status', { id, groupPreview: 1 });
            const preview = response?.data;
            if (!response?.success || preview?.id !== id || !Array.isArray(preview.channels) ||
                !/^[0-9a-f]{48}$/.test(preview.groupToken || '') || preview.provider?.enabled !== false)
                throw new Error('整组预览读取失败，未启用设备');
            const channels = preview.channels.slice().sort((a, b) => Number(a.params?.channel) - Number(b.params?.channel));
            if (!channels.length || channels.length > 16 || channels.some((p) => p.enabled !== false))
                throw new Error('请先以禁用状态保存所有通道，再整组启用');
            const ids = new Set(), pins = new Set();
            for (const p of channels) {
                const channel = p.params?.channel;
                if (typeof p.id !== 'string' || !p.id || ids.has(p.id) || pins.has(channel) ||
                    ![11, 12].includes(p.type) || p.params?.providerId !== id ||
                    !Number.isInteger(channel) || channel < 0 || channel > 15 ||
                    (p.type === 11 && [7, 15].includes(channel)) ||
                    (p.type === 12 && (![0, 1].includes(p.params.initialState) || typeof p.params.invertLogic !== 'boolean')))
                    throw new Error('通道配置不完整或重复，请重新配置');
                ids.add(p.id); pins.add(channel);
            }
            if (preview.reset?.enabled !== true || preview.reset.params?.nativeControlEligible !== true ||
                preview.reset.pins?.length !== 1 ||
                (preview.provider.params?.busPeripheralId && preview.bus?.enabled !== true))
                throw new Error('请先启用有效的专用 RESET 输出与引用总线');
            if (![7, 15].every((ch) => channels.some((p) => Number(p.type) === 12 && Number(p.params.channel) === ch)))
                throw new Error('必须先显式配置 A7（7）和 B7（15）为数字输出');
            return new Promise((resolve, reject) => {
                const dialog = document.createElement('dialog');
                const title = document.createElement('h3');
                title.textContent = '选择 MCP23017 整组通道';
                const note = document.createElement('p');
                note.textContent = '启用会复位芯片并写入选定输出的初始电平。请核对 RESET 接线、负载与下列电平；A7、B7 必须选中。不选的通道保持禁用。';
                const wiring = document.createElement('p');
                const busPins = preview.bus?.pins || preview.provider.pins || [];
                wiring.textContent = '设备 ' + id + ' · 地址 ' + preview.provider.params?.address +
                    ' · SDA/SCL ' + busPins.join('/') + ' · ' + (preview.bus?.frequency || 100000) + ' Hz' +
                    ' · RESET ' + preview.reset.id + ' / GPIO ' + preview.reset.pins[0];
                const form = document.createElement('form');
                form.method = 'dialog';
                form.append(title, note, wiring);
                const choices = channels.map((p) => {
                    const row = document.createElement('p');
                    const label = document.createElement('label');
                    const input = document.createElement('input');
                    input.type = 'checkbox';
                    const output = Number(p.type) === 12;
                    const high = (Number(p.params.initialState) === 1) !== !!p.params.invertLogic;
                    label.append(input, document.createTextNode(' P' + p.params.channel + ' · ' + (p.name || p.id) +
                        ' (' + p.id + ') · ' + (output ? '输出初始物理电平 ' + (high ? '高' : '低') : '输入') +
                        ([7, 15].includes(Number(p.params.channel)) ? ' · 必选' : '')));
                    row.appendChild(label); form.appendChild(row);
                    return { input, peripheral: p };
                });
                const warning = document.createElement('p');
                warning.setAttribute('role', 'alert');
                const cancel = document.createElement('button');
                cancel.type = 'button'; cancel.textContent = '取消';
                const submit = document.createElement('button');
                submit.type = 'submit'; submit.textContent = '确认接线与电平，启用所选通道';
                const finish = (value) => { dialog.remove(); resolve(value); };
                cancel.addEventListener('click', () => finish(null));
                dialog.addEventListener('cancel', (event) => { event.preventDefault(); finish(null); });
                form.addEventListener('submit', (event) => {
                    event.preventDefault();
                    const selected = choices.filter((choice) => choice.input.checked).map((choice) => choice.peripheral);
                    if (![7, 15].every((ch) => selected.some((p) => Number(p.params.channel) === ch))) {
                        warning.textContent = '请核对并选中必需的 A7、B7 输出。'; return;
                    }
                    finish({ channelIds: selected.map((p) => p.id), groupToken: preview.groupToken });
                });
                form.append(warning, cancel, submit); dialog.appendChild(form); document.body.appendChild(dialog);
                if (typeof dialog.showModal !== 'function') {
                    dialog.remove(); reject(new Error('当前浏览器不支持整组确认，请更换浏览器')); return;
                }
                dialog.showModal();
            });
        },

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
            ['prepare', 'start', 'set', 'enable', 'disable', 'stop'].forEach((action) => {
                document.getElementById('peripheral-pca9685-' + action + '-btn')?.addEventListener('click', () => this.runPeripheralPca9685(action));
            });
            document.getElementById('peripheral-pca9685-confirm')?.addEventListener('change', () => this._updatePca9685Controls());
            [
                ['peripheral-sd-mount-btn', 'mount'],
                ['peripheral-sd-unmount-btn', 'unmount'],
                ['peripheral-sd-recover-btn', 'recover'],
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
                'peripheral-encoder-reset-btn', 'peripheral-sd-mount-btn', 'peripheral-sd-unmount-btn', 'peripheral-sd-recover-btn',
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
                    'peripheral-sd-unmount-btn', 'peripheral-sd-recover-btn', 'peripheral-sd-write-btn', 'peripheral-sd-delete-btn',
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
            this._updatePca9685Controls();
        },

        async openPeripheralIoModal(id) {
            const session = {};
            this._periphIoSession = session;
            this._periphPcaReview = null;
            this._periphPcaStatus = null;
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
                pca9685: 'peripheral-pca9685-panel',
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
                    this._setPeripheralIoText('peripheral-uart-read-quality', '尚未读取');
                    this.updatePeripheralUartPayloadState();
                } else if (this._periphIoKind === 'encoder') {
                    this._setPeripheralIoText('peripheral-io-title', '编码器读数');
                    this._setPeripheralIoText('peripheral-encoder-resolution-display', data.params?.resolution);
                } else if (this._periphIoKind === 'pca9685') {
                    this._setPeripheralIoText('peripheral-io-title', 'PCA9685 PWM');
                    this._setPeripheralIoText('peripheral-pca9685-review', '尚未准备');
                    const checkbox = document.getElementById('peripheral-pca9685-confirm');
                    if (checkbox) checkbox.checked = false;
                    this._renderPeripheralPca9685(null);
                } else if (this._periphIoKind === 'sd') {
                    this._setPeripheralIoText('peripheral-io-title', 'SD 卡文件');
                    this._renderPeripheralSdData({statusAvailable: false});
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
                    this._clearPeripheralDs1302Reading('尚未读取');
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
            this._periphPcaReview = null;
            this._periphPcaStatus = null;
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
                var localReady = typeof data.localReady === 'boolean' ? data.localReady : data.connected;
                this._setPeripheralIoText('peripheral-uart-connected', localReady === true ? '本地就绪；对端未验证' : localReady === false ? '本地未就绪' : '--');
                this._setPeripheralIoText('peripheral-uart-port', Number(data.port) >= 0 ? ('UART' + data.port) : '--');
                this._setPeripheralIoText('peripheral-uart-framing', data.framing);
                this._setPeripheralIoText('peripheral-uart-rx-available', data.rxAvailable !== undefined ? (data.rxAvailable + ' bytes') : '--');
                this._setPeripheralIoText('peripheral-uart-bytes-sent', data.bytesSent !== undefined ? (data.bytesSent + ' bytes') : '--');
                this._setPeripheralIoText('peripheral-uart-bytes-received', data.bytesReceived !== undefined ? (data.bytesReceived + ' bytes') : '--');
                if (data.length !== undefined) {
                    var length = Number(data.length);
                    if (!Number.isInteger(length) || length < 0 || length > 128 ||
                        (data.readQuality !== undefined && !['bytes', 'no_data'].includes(data.readQuality)) ||
                        (data.readQuality === 'bytes' && !length) || (data.readQuality === 'no_data' && length)) {
                        throw new Error('UART 接收结果不一致，未追加数据');
                    }
                    if (length) {
                        this._periphUartTextLog = this._appendPeripheralIoLog(this._periphUartTextLog, data.text || '[非文本数据]');
                        this._periphUartHexLog = this._appendPeripheralIoLog(this._periphUartHexLog, data.hex || '');
                    }
                    this._setPeripheralIoText('peripheral-uart-text-output', this._periphUartTextLog || '[无新数据]');
                    this._setPeripheralIoText('peripheral-uart-hex-output', this._periphUartHexLog || '[无新数据]');
                    this._setPeripheralIoText('peripheral-uart-read-quality', data.readQuality === undefined
                        ? '旧固件未提供接收质量；历史保留'
                        : length ? '本次 ' + length + ' 字节；未验证完整协议帧' : '本次无新字节；历史保留');
                }
            } else if (this._periphIoKind === 'encoder') {
                this._setPeripheralIoText('peripheral-encoder-count', data.count);
                this._setPeripheralIoText('peripheral-encoder-delta', data.delta);
                var direction = Number(data.direction);
                this._setPeripheralIoText('peripheral-encoder-direction', direction > 0 ? '正向 (+1)' : (direction < 0 ? '反向 (-1)' : '静止 (0)'));
                this._setPeripheralIoText('peripheral-encoder-frequency', data.frequencyHz !== undefined ? (Number(data.frequencyHz).toFixed(2) + ' Hz') : '--');
                this._setPeripheralIoText('peripheral-encoder-rpm', data.rpm !== undefined ? (Number(data.rpm).toFixed(2) + ' RPM') : '--');
            } else if (this._periphIoKind === 'pca9685') {
                this._renderPeripheralPca9685(data.pca9685);
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
                var readData = this._periphIoType !== 2 && this._periphIoType !== 57 && (this._periphIoKind === 'encoder' || consumeData === true);
                var path = readData ? '/api/peripherals/read' : '/api/peripherals/status';
                var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                var params = { id: this._periphIoId };
                if (readData && this._periphIoKind === 'sd') params.action = 'status';
                if (readData && this._periphIoKind === 'i2c-sensor') {
                    params.action = 'read';
                    params.channel = document.getElementById('peripheral-i2c-channel')?.value || '';
                }
                if (this._periphIoKind === 'ds1302') this._clearPeripheralDs1302Reading('读取中');
                if (this._periphIoKind === 'sd') this._renderPeripheralSdData({statusAvailable: false});
                if (this._periphIoKind === 'uart' && readData) this._setPeripheralIoText('peripheral-uart-read-quality', '读取中；历史保留');
                var res = await getter(path, params);
                if (this._periphIoSession !== session) return;
                if (!res || !res.success || !res.data) throw new Error(res?.message || res?.error || '读取失败');
                if (this._periphIoKind === 'uart' && readData && res.data.length === undefined) throw new Error('UART 接收长度未确认');
                this._renderPeripheralIoReading(res.data);
            } catch (err) {
                if (this._periphIoSession !== session) return;
                if (this._periphIoKind === 'i2c-sensor') this._setPeripheralIoText('peripheral-i2c-value', '读取失败');
                if (this._periphIoKind === 'ds1302') this._clearPeripheralDs1302Reading('读取失败；旧日历不可作为当前值');
                if (this._periphIoKind === 'sd') this._renderPeripheralSdData({statusAvailable: false});
                if (this._periphIoKind === 'pca9685') this._renderPeripheralPca9685(null);
                if (this._periphIoKind === 'uart') {
                    this._pausePeripheralUartPolling();
                    this._setPeripheralIoText('peripheral-uart-connected', '--');
                    this._setPeripheralIoText('peripheral-uart-rx-available', '--');
                    this._setPeripheralIoText('peripheral-uart-read-quality', readData
                        ? '读取未确认，RX可能已消费；轮询已停止，历史保留' : '状态获取失败；轮询已停止，历史保留');
                }
                this._setPeripheralIoError(err?.data?.message || err?.data?.error || err?.message || '读取失败');
            } finally {
                if (this._periphIoSession === session) this._setPeripheralIoBusy(false);
            }
        },

        _updatePca9685Controls() {
            const blocked = this._periphIoKind !== 'pca9685' || this._periphIoBusy || this._periphIoBlocked || !this.isDeveloperModeEnabled();
            const state = this._periphPcaStatus?.state;
            const ready = state === 'ready_disabled' || state === 'enabled';
            const review = this._periphPcaReview;
            const fresh = review && Date.now() < review.expires && state === 'awaiting_power_cycle';
            ['prepare', 'start', 'set', 'enable', 'disable', 'stop'].forEach((action) => {
                const el = document.getElementById('peripheral-pca9685-' + action + '-btn');
                if (!el) return;
                el.disabled = !!blocked || (action === 'prepare' && !['awaiting_review', 'closed'].includes(state)) ||
                    (action === 'start' && (!fresh || !document.getElementById('peripheral-pca9685-confirm')?.checked)) ||
                    (['set', 'enable'].includes(action) && !ready);
            });
        },

        _renderPeripheralPca9685(status) {
            const labels = {awaiting_review: '等待准备', awaiting_power_cycle: '等待本次重新上电确认', releasing_oe: '正在释放 OE',
                waking: '振荡器启动中', ready_disabled: '就绪，输出禁用', enabled: '输出已启用（实物未验证）',
                faulted: '故障，需停止后重新准备', closing: '正在停止', closed: '已释放', not_ready: '未就绪'};
            const valid = status && Object.prototype.hasOwnProperty.call(labels, status.state) &&
                typeof status.outputsEnabled === 'boolean' && status.outputsEnabled === (status.state === 'enabled') &&
                status.physicalOutputVerified === false;
            this._periphPcaStatus = valid ? status : null;
            if (!valid || status.state !== 'awaiting_power_cycle') this._periphPcaReview = null;
            const details = valid ? [labels[status.state], '累计失败：' + (status.failures ?? '--'),
                '首个错误：' + (status.firstError || '无'), '最近错误：' + (status.lastError || '无'),
                '计算频率：' + (status.frequencyCalculatedHz || '--') + ' Hz（非实测）'].join('；') : '状态未确认；仅刷新查询，不自动重试动作';
            this._setPeripheralIoText('peripheral-pca9685-state', details);
            this._updatePca9685Controls();
        },

        async runPeripheralPca9685(action) {
            if (this._periphIoKind !== 'pca9685' || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked || !this.isDeveloperModeEnabled()) return;
            const session = this._periphIoSession, id = this._periphIoId;
            const payload = {id, action};
            if (action === 'start') {
                const review = this._periphPcaReview;
                if (!review || Date.now() >= review.expires || !document.getElementById('peripheral-pca9685-confirm')?.checked) {
                    this._setPeripheralIoError('请先准备并完成本次重新上电确认'); return;
                }
                payload.token = review.token; payload.powerCycleConfirmed = 'true';
            } else if (action === 'set') {
                payload.action = document.getElementById('peripheral-pca9685-mode')?.value;
                const channelText = document.getElementById('peripheral-pca9685-channel')?.value.trim() || '';
                const valueText = document.getElementById('peripheral-pca9685-value')?.value.trim() || '';
                payload.channel = Number(channelText); payload.value = Number(valueText);
                if (!['duty', 'pulse', 'angle'].includes(payload.action) || !channelText || !valueText ||
                    !Number.isInteger(payload.channel) || payload.channel < 0 || payload.channel > 15 || !Number.isFinite(payload.value)) {
                    this._setPeripheralIoError('通道或设置值无效'); return;
                }
            } else if (!['prepare', 'enable', 'disable', 'stop'].includes(action)) return;
            if (action === 'enable' && !await FBConfirm({ title: '启用 PCA9685 输出', message: '确认接线与负载安全，并启用本次 PCA9685 输出？', danger: true })) return;
            // Consume browser confirmation before sending. An unknown response
            // must not offer the same initialization token for replay.
            this._periphPcaReview = null;
            const checkbox = document.getElementById('peripheral-pca9685-confirm');
            if (checkbox) checkbox.checked = false;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            try {
                const res = await apiPost('/api/peripherals/write', payload);
                if (this._periphIoSession !== session) return;
                if (!res?.data || res.data.id !== id) throw new Error('操作响应身份未确认');
                this._renderPeripheralPca9685(res.data.statusAvailable === true ? res.data.status : null);
                if (!res.success) throw Object.assign(new Error(res.error || '操作未确认'), {data: res});
                if (action === 'prepare') {
                    const review = res.data.review;
                    if (this._periphPcaStatus?.state !== 'awaiting_power_cycle' || review?.id !== id ||
                        !/^[0-9a-f]{32}$/.test(review?.token || '') || review.validForMs !== 60000 || review.oePadHigh !== true ||
                        review.outputsEnabled !== false || review.physicalPowerVerified !== false) throw new Error('准备结果不完整，请停止后重新准备');
                    this._periphPcaReview = {token: review.token, expires: Date.now() + review.validForMs};
                    this._setPeripheralIoOutput('peripheral-pca9685-review', {sda: review.sda, scl: review.scl,
                        i2cFrequency: review.i2cFrequency, params: review.params, 提示: 'OE 已请求高电平；请现在对芯片重新上电，60 秒内确认'});
                }
            } catch (err) {
                if (this._periphIoSession !== session) return;
                this._periphPcaReview = null;
                this._renderPeripheralPca9685(null);
                const result = err?.data;
                const helper = window.PeriphExecRunValuePrompt;
                const explained = result?.data?.id === id && result.data.control?.evaluated === true &&
                    result.data.control.targetId === id &&
                    typeof helper?._manualOutputMessage === 'function';
                this._setPeripheralIoError(explained ? helper._manualOutputMessage(result) + '；请刷新状态' :
                    (result?.error || err?.message || '操作未确认') + '；不自动重试，请刷新状态');
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
            if (this._periphIoType !== 1 || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked || !this.isDeveloperModeEnabled()) return;
            const session = this._periphIoSession, id = this._periphIoId;
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
                    id: id,
                    data: info.value,
                    hex: info.mode === 'hex' ? 1 : 0
                });
                if (this._periphIoSession !== session) return;
                if (!res || !res.success) throw new Error(res?.message || res?.error || '发送失败');
                Notification.success('本地驱动已接受 ' + info.bytes + ' bytes；对端接收与执行未验证', 'UART 终端');
                this._setPeripheralIoBusy(false);
                await this.refreshPeripheralIo(false);
            } catch (err) {
                if (this._periphIoSession !== session) return;
                this._pausePeripheralUartPolling();
                this._setPeripheralIoError((err?.data?.message || err?.data?.error || err?.message || '发送失败') + '；未确认完整发送，可能已部分生效，请核对对端后再决定是否重发');
                this._setPeripheralIoBusy(false);
            }
        },

        async resetPeripheralEncoder() {
            if (this._periphIoType !== 43 || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            if (!await FBConfirm({ title: '编码器清零', message: '确定将编码器累计计数清零吗？该操作不可恢复。', danger: true })) return;
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

        _pausePeripheralUartPolling() {
            this._stopPeripheralIoPolling();
            var poll = document.getElementById('peripheral-uart-poll-interval');
            if (poll) poll.value = '0';
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
            if (data.statusAvailable !== undefined || data.mounted !== undefined) {
                var known = data.statusAvailable === true && typeof data.mounted === 'boolean';
                this._setPeripheralIoText('peripheral-sd-mounted', known ? (data.mounted ? '已挂载' : '未挂载') : '状态未确认');
                var valid = known && data.mounted && data.capacityQuality === 'last_operation_snapshot'
                    && [data.totalBytes, data.usedBytes, data.capacityAgeMs].every(v => Number.isSafeInteger(v) && v >= 0)
                    && data.totalBytes > 0 && data.usedBytes <= data.totalBytes;
                if (!valid) data = {...data, cardType: '', totalBytes: undefined, usedBytes: undefined};
                this._setPeripheralIoText('peripheral-sd-card-type', data.cardType || '--');
                this._setPeripheralIoText('peripheral-sd-total', this._formatPeripheralBytes(data.totalBytes));
                this._setPeripheralIoText('peripheral-sd-used', this._formatPeripheralBytes(data.usedBytes));
                this._setPeripheralIoText('peripheral-sd-free', this._formatPeripheralBytes(valid ? data.totalBytes - data.usedBytes : NaN));
                this._setPeripheralIoText('peripheral-sd-filesystem', valid ? '上次操作 · ' + data.capacityAgeMs + ' ms 前' : '不可用／质量未知');
                const recoveryKnown = (known || (data.configurationAvailable === true && data.configured === true)) && typeof data.recoverySuggested === 'boolean' &&
                    [data.ioFailures, data.recoveryAttempts, data.recoveries].every(v => Number.isSafeInteger(v) && v >= 0 && v <= 4294967295) &&
                    data.recoveries <= data.recoveryAttempts && typeof data.lastFailureCode === 'string';
                this._setPeripheralIoText('peripheral-sd-recovery', recoveryKnown ?
                    (data.recoverySuggested ? '发生过未确认的卡操作；检查接线/介质后可手动重新挂载。' : '当前没有恢复建议；挂载状态不保证介质健康。') +
                    ' 本配置周期：I/O故障 ' + data.ioFailures + '，手动恢复 ' + data.recoveries + '/' + data.recoveryAttempts +
                    (data.lastFailureCode ? '，最近故障 ' + data.lastFailureCode : '') +
                    '；不会重放失败的文件操作，历史故障不因恢复清零。' : '恢复状态未确认（旧固件、忙碌或状态缺失）');
                if (data.driverError) this._setPeripheralIoError(data.driverError);
            }
            if (Array.isArray(data.entries)) {
                const entryValid = entry => entry && typeof entry.directory === 'boolean' &&
                    typeof entry.path === 'string' && entry.path.length <= 128 &&
                    /^\/fastbee\/[A-Za-z0-9_./-]+$/.test(entry.path) &&
                    !entry.path.includes('..') && !entry.path.includes('//') &&
                    Number.isSafeInteger(entry.size) && entry.size >= 0 && (!entry.directory || entry.size === 0);
                if (!Number.isSafeInteger(data.count) || data.count !== data.entries.length ||
                    data.entries.length > 32 || typeof data.truncated !== 'boolean' ||
                    (data.truncated && data.entries.length !== 32) || !data.entries.every(entryValid)) {
                    throw new Error('SD_LIST_UNCONFIRMED: 目录条目或截断状态未确认，保留原列表');
                }
                var lines = data.entries.map((entry) => {
                    var label = (entry.directory ? '[DIR] ' : '[FILE] ') + (entry.path || entry.name || '--');
                    return entry.directory ? label : (label + '  ' + this._formatPeripheralBytes(entry.size));
                });
                if (data.truncated) lines.push('[仅显示前 32 项，目录中还有更多条目]');
                this._setPeripheralIoOutput('peripheral-sd-output', lines.join('\n') || '[目录为空]');
            } else if (data.content !== undefined) {
                var byteLength = typeof data.content === 'string'
                    ? (typeof TextEncoder === 'function' ? new TextEncoder().encode(data.content).length
                        : unescape(encodeURIComponent(data.content)).length) : -1;
                if (!Number.isSafeInteger(data.length) || data.length < 0 || data.length > 4096 ||
                    byteLength !== data.length) {
                    throw new Error('SD_READ_UNCONFIRMED: 文件内容或字节数未确认，保留原草稿');
                }
                var content = document.getElementById('peripheral-sd-content');
                if (content) content.value = data.content;
                this._updateSdContentCount();
                this._setPeripheralIoOutput('peripheral-sd-output', '已读取 ' + data.length + ' bytes');
            }
        },

        async runPeripheralSdOperation(action) {
            if (this._periphIoKind !== 'sd' || !this._periphIoId || this._periphIoBusy || this._periphIoBlocked) return;
            var normalized = action === 'read-file' ? 'read' : action;
            if (!['status', 'list', 'read', 'mount', 'recover', 'unmount', 'write-file', 'delete-file'].includes(normalized)) return;
            var mutating = ['mount', 'recover', 'unmount', 'write-file', 'delete-file'].indexOf(normalized) >= 0;
            if (mutating && !this.guardDeveloperModeAction()) return;
            var path = (document.getElementById('peripheral-sd-path')?.value || '/').trim();
            if (!path) path = '/';
            if (normalized === 'write-file' && this._updateSdContentCount() > 4096) {
                this._setPeripheralIoError('文件内容不能超过 4096 bytes');
                return;
            }
            if (normalized === 'delete-file' && !await FBConfirm({ title: '删除 SD 文件', message: '确定删除 SD 沙箱中的 ' + path + ' 吗？该操作不可恢复。', danger: true })) return;
            if (normalized === 'recover' && !await FBConfirm({ title: '重新挂载 SD', message: '检查 SD 介质和接线后重新挂载？不会重放之前失败的文件操作，也不保证文件已落盘。' })) return;
            const session = this._periphIoSession;
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
                    const payload = {id: this._periphIoId, action: normalized};
                    if (normalized === 'write-file') Object.assign(payload, {path: path,
                        content: document.getElementById('peripheral-sd-content')?.value || '', append: 0});
                    if (normalized === 'delete-file') Object.assign(payload, {path: path, confirm: 'DELETE'});
                    res = await apiPost('/api/peripherals/write', payload);
                }
                if (this._periphIoSession !== session) return;
                if (!res || res.success !== true) throw new Error(res?.message || res?.error || 'SD 操作结果未确认；请先核实，不要重复写入');
                var data = res.data || res;
                if (normalized === 'read' && data.content === undefined)
                    throw new Error('SD_READ_UNCONFIRMED: 未返回完整文件，保留原草稿');
                if (normalized === 'list' && !Array.isArray(data.entries))
                    throw new Error('SD_LIST_UNCONFIRMED: 未返回目录列表，保留原列表');
                this._renderPeripheralSdData(data);
                if (!Array.isArray(data.entries) && data.content === undefined) {
                    this._setPeripheralIoOutput('peripheral-sd-output', res.message || '操作成功');
                }
                if (mutating) Notification.success(res.message || 'SD 操作成功', 'SD 卡');
                this._setPeripheralIoBusy(false);
                if (mutating) await this.refreshPeripheralIo(false);
            } catch (err) {
                if (this._periphIoSession !== session) return;
                this._renderPeripheralSdData({statusAvailable: false});
                this._setPeripheralIoError(this._peripheralIoErrorMessage(err, 'SD 操作失败'));
                this._setPeripheralIoBusy(false);
            }
        },

        _normalizePeripheralHex(value) {
            return String(value || '').replace(/[\s:-]+/g, '').toUpperCase();
        },

        _renderPeripheralRfidData(data) {
            if (data.statusAvailable === false) {
                this._setPeripheralIoText('peripheral-rfid-ready', '忙碌，状态待刷新');
                this._setPeripheralIoText('peripheral-rfid-uid', '--');
                this._setPeripheralIoText('peripheral-rfid-card-type', '--');
                this._setPeripheralIoError(data.driverError || 'SPI_BUSY: RFID 状态暂不可用');
                return;
            }
            this._setPeripheralIoError(data.driverError || '');
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
            if (action === 'write-block' && !await FBConfirm({ title: '写入 RFID 数据块', message: '确定写入 RFID 数据块 ' + block + ' 并执行写后校验吗？', danger: true })) return;
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
                return (!channel.mode || Number(channel.mode) === mode + 1) &&
                    (!channel.singleMuxOnly || !Number(config.params?.scanMask)) &&
                    (channel.scanMux === undefined || !!(Number(config.params?.scanMask) & (1 << Number(channel.scanMux))));
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

        _clearPeripheralDs1302Reading(reason) {
            ['ready', 'running', 'current'].forEach(key => this._setPeripheralIoText('peripheral-ds1302-' + key, '--'));
            this._setPeripheralIoOutput('peripheral-ds1302-output', reason);
        },

        _renderPeripheralDs1302Data(data) {
            this._setPeripheralIoText('peripheral-ds1302-ready', data.initialized === undefined ? '--' : (data.initialized ? '已初始化' : '未初始化'));
            var quality = data.running === false && data.quality === 'halted' ? '已暂停；非运行时钟' :
                (data.running === true && data.quality === 'calendar_unverified' && data.timeBasis === 'device_calendar_unzoned'
                    ? '运行中；时间源未核验' : '质量未知');
            this._setPeripheralIoText('peripheral-ds1302-running', quality);
            var formatted = data.formatted;
            if (!formatted && data.year !== undefined) {
                var pad = function(value) { return String(value).padStart(2, '0'); };
                formatted = data.year + '-' + pad(data.month) + '-' + pad(data.day) + ' ' +
                    pad(data.hour) + ':' + pad(data.minute) + ':' + pad(data.second);
            }
            this._setPeripheralIoText('peripheral-ds1302-current', formatted || '--');
            this._setPeripheralIoOutput('peripheral-ds1302-output', { calendar: formatted || null, quality: quality, timeBasis: data.timeBasis || 'unknown' });
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
            if (action === 'stop' && !await FBConfirm({ title: '暂停 DS1302 计时', message: '确定暂停 DS1302 计时吗？' })) return;
            const session = this._periphIoSession;
            this._setPeripheralIoBusy(true);
            this._setPeripheralIoError('');
            this._clearPeripheralDs1302Reading('写入结果待确认');
            try {
                var res = await apiPost('/api/peripherals/write', payload);
                if (this._periphIoSession !== session) return;
                if (!res || res.success !== true) throw new Error(res?.message || res?.error || 'DS1302 操作失败');
                this._setPeripheralIoOutput('peripheral-ds1302-output', res.message || '操作成功');
                this._setPeripheralIoBusy(false);
                await this.refreshPeripheralIo(true);
            } catch (err) {
                if (this._periphIoSession !== session) return;
                this._clearPeripheralDs1302Reading('写入结果未确认，请重新读取核验');
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
