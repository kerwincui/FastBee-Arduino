/**
 * 外设接口管理模块
 * 包含外设列表、新增/编辑/删除外设、类型参数配置
 */
/* eslint-disable */
// Define global t() function if not already defined by i18n
const t = (typeof window.t === 'function') ? window.t : (key, defaultValue) => defaultValue;
/* eslint-enable */
(function() {
    AppState.registerModule('peripherals', {
        _restoreRtcBootSeed(params) {
            const seed = document.getElementById('ds1302-boot-seed');
            const offset = document.getElementById('ds1302-utc-offset');
            if (seed) seed.checked = params.bootSeed === true;
            if (offset) offset.value = Number.isInteger(params.utcOffsetMinutes) &&
                Math.abs(params.utcOffsetMinutes) <= 840 ? String(params.utcOffsetMinutes) : '';
        },

        // ============ 状态变量 ============
        _periphCurrentPage: 1,
        _periphPageSize: 10,
        _periphTotalCount: 0,
        _periphProfile: null,
        _periphHardware: null,
        _periphTypeCatalog: null,
        _periphTypeCatalogLoading: false,
        _periphPreflightFingerprint: '',
        _periphPreflightAcknowledgements: 0,
        _periphFormSession: null,
        _periphIoId: '',
        _periphIoType: 0,
        _periphIoKind: '',
        _periphIoBusy: false,
        _periphIoPollTimer: null,
        _periphUartTextLog: '',
        _periphUartHexLog: '',
        _periphEventsBound: false,

        // ============ 事件绑定 ============
        setupPeripheralsEvents() {
            if (this._periphEventsBound) return;
            // 新增外设按钮（页面元素，非模态窗内）
            var addPeripheralBtn = document.getElementById('add-peripheral-btn');
            if (addPeripheralBtn) addPeripheralBtn.addEventListener('click', () => this.openPeripheralModal());
            // 外设过滤器
            var peripheralFilter = document.getElementById('peripheral-filter-type');
            if (peripheralFilter) peripheralFilter.addEventListener('change', (e) => { this._periphCurrentPage = 1; this.loadPeripherals(e.target.value); });
            // 兼容旧版：GPIO 新增按钮
            var addGpioBtn = document.getElementById('add-gpio-btn');
            if (addGpioBtn) addGpioBtn.addEventListener('click', () => this.openPeripheralModal());
            var tableBody = document.getElementById('peripheral-table-body');
            if (tableBody) {
                tableBody.addEventListener('click', (event) => this._handlePeripheralTableClick(event));
            }
            // 刷新按钮
            var refreshBtn = document.getElementById('peripheral-refresh-btn');
            if (refreshBtn) refreshBtn.addEventListener('click', () => this._refreshPeripheralList());
            var resourcesBtn = document.getElementById('peripheral-resources-btn');
            if (resourcesBtn) resourcesBtn.addEventListener('click', () => this._openPeripheralResources());
            // 注册模态窗事件绑定器（模态窗 DOM 加载后由 _loadModals 触发）
            if (typeof this._registerModalBinder === 'function') {
                this._registerModalBinder('peripherals', () => this._bindPeripheralModalEvents());
            }
            if (typeof this.onPageHide === 'function') {
                this.onPageHide('peripherals-io-poll', () => {
                    this._periphResourceOpen = null;
                    if (typeof this._stopPeripheralIoPolling === 'function') this._stopPeripheralIoPolling();
                    if (typeof this.closePeripheralResources === 'function') this.closePeripheralResources();
                });
            }
            // The capability catalog is large on constrained targets and is
            // only needed by the add/edit modal. Loading it here competes with
            // the first-screen peripheral list and can push the next request
            // into WebMemoryGuard. openPeripheralModal() loads it on demand.
            this._periphEventsBound = true;
        },

        /**
         * 绑定模态窗内的事件（模态窗 DOM 已就绪后调用）
         * 由 _loadModals → _rebindAllModalEvents 触发
         */
        _bindPeripheralModalEvents() {
            if (this._periphModalBound) return;
            // 外设模态框
            ['close-peripheral-modal', 'cancel-peripheral-btn', 'close-gpio-modal', 'cancel-gpio-btn'].forEach(id => {
                document.getElementById(id)?.addEventListener('click', () => this.closePeripheralModal());
            });
            var savePeripheralBtn = document.getElementById('save-peripheral-btn');
            if (savePeripheralBtn) savePeripheralBtn.addEventListener('click', () => this._savePeripheralConfigWithModule());
            var peripheralTypeInput = document.getElementById('peripheral-type-input');
            if (peripheralTypeInput) peripheralTypeInput.addEventListener('change', (e) => this.onPeripheralTypeChange(e.target.value));
            var adcSignalMode = document.getElementById('adc-signal-mode');
            if (adcSignalMode) adcSignalMode.addEventListener('change', (e) => this._applyAnalogSignalDefaults(e.target.value));
            var pinsInput = document.getElementById('peripheral-pins-input');
            if (pinsInput) pinsInput.addEventListener('blur', () => {
                this._loadPeripheralSaveModule(function() { this._validatePinsInline(); });
            });
            var peripheralForm = document.getElementById('peripheral-form');
            if (peripheralForm) {
                peripheralForm.addEventListener('input', () => this._resetPeripheralPreflight());
                peripheralForm.addEventListener('change', () => this._resetPeripheralPreflight());
            }
            var genericSensorDriver = document.getElementById('generic-sensor-driver');
            if (genericSensorDriver) genericSensorDriver.addEventListener('change', () => this._updateGenericSensorDriverFields(true));
            ['generic-sensor-bus-id', 'io-expander-bus-id'].forEach((id) => {
                document.getElementById(id)?.addEventListener('change', () => this._updatePeripheralPinRoleHint(document.getElementById('peripheral-type-input')?.value));
            });
            document.getElementById('io-expander-driver')?.addEventListener('change', () => this._updateIoExpanderFields(true));
            ['gpio-io-source', 'gpio-provider-id'].forEach((id) => {
                document.getElementById(id)?.addEventListener('change', () => this._updateGpioSourceFields());
            });
            var sdcardInterface = document.getElementById('sdcard-interface');
            if (sdcardInterface) sdcardInterface.addEventListener('change', () => this._updateSdcardPinHints());
            ['display-touch-backend', 'display-backend', 'display-driver'].forEach(id => {
                document.getElementById(id)?.addEventListener('change', () => this._updateDisplayFields());
            });
            document.getElementById('display-backlight')?.addEventListener('input', () => this._updateDisplayFields());
            // 兼容旧版 GPIO 模态框
            var saveGpioBtn = document.getElementById('save-gpio-btn');
            if (saveGpioBtn) saveGpioBtn.addEventListener('click', () => this._savePeripheralConfigWithModule());
            this._syncPeripheralTypeSelect(this._periphTypeCatalog);
            this._periphModalBound = true;
        },

        // Keep the common 0-10 V / 0-20 mA / 4-20 mA path one-click simple.
        // Advanced users can still override the inferred input range below.
        _applyAnalogSignalDefaults(modeValue) {
            var mode = parseInt(modeValue, 10);
            var ranges = {
                0: [0, 0],
                1: [0, 10],
                2: [0, 20],
                3: [4, 20],
                4: [0, 4095]
            };
            var range = ranges[mode] || ranges[0];
            var minEl = document.getElementById('adc-input-min');
            var maxEl = document.getElementById('adc-input-max');
            var conversionEl = document.getElementById('adc-conversion-enabled');
            if (minEl) minEl.value = String(range[0]);
            if (maxEl) maxEl.value = String(range[1]);
            if (conversionEl) conversionEl.checked = mode !== 0;
            if (mode === 4) {
                var rawMin = document.getElementById('adc-raw-min');
                var rawMax = document.getElementById('adc-raw-max');
                if (rawMin) rawMin.value = '0';
                if (rawMax) rawMax.value = '4095';
            }
        },

        _handlePeripheralTableClick(event) {
            var button = event.target.closest('[data-peripheral-action]');
            if (!button) return;
            var action = button.getAttribute('data-peripheral-action');
            var id = button.getAttribute('data-id');
            if (!action || !id) return;
            if (action === 'diagnose') {
                button.closest('details')?.removeAttribute('open');
                this._openPeripheralIoWorkspace(id);
                return;
            }
            if (!this.guardDeveloperModeAction()) return;
            if (action === 'edit') this.editPeripheral(id);
            else if (action === 'toggle') this.togglePeripheral(id);
            else if (action === 'delete') this.deletePeripheral(id);
        },

        // Shared lazy-loader; each manager retains its existing initialization.
        _initPeripheralRemotePanel(module, method) {
            if (typeof ModuleLoader === 'undefined' || typeof ModuleLoader.loadModule !== 'function') return;
            ModuleLoader.loadModule(module, () => {
                if (typeof this[method] === 'function') this[method]();
            });
        },

        _initIrManagePanel() { this._initPeripheralRemotePanel('ir-remote', 'initIrManage'); },
        _initRfManagePanel() { this._initPeripheralRemotePanel('rf-remote', 'initRfManage'); },

        _renderPeripheralActionButton(action, id, label, className, requiresDeveloper = true, title = '') {
            var locked = requiresDeveloper && !this.isDeveloperModeEnabled();
            var attrs = locked ? ' disabled title="' + escapeHtml(this.getDeveloperModeDisabledText()) + '"' : (title ? ' title="' + escapeHtml(title) + '"' : '');
            var lockClass = locked ? ' dev-mode-locked' : '';
            return '<button class="fb-btn fb-btn-sm fb-btn-compact ' + className + lockClass + '" data-peripheral-action="' + action + '" data-id="' + escapeHtml(id) + '"' + attrs + '>' + label + '</button>';
        },

        _getPeripheralTypeDescriptor(typeValue) {
            var type = Number(typeValue);
            if (!Array.isArray(this._periphTypeCatalog) || !Number.isFinite(type)) return null;
            return this._periphTypeCatalog.find(function(item) {
                return Number(item && (item.value ?? item.type)) === type;
            }) || null;
        },

        _getPeripheralDiagnosticKind(peripheral) {
            var type = Number(peripheral && peripheral.type);
            var descriptor = this._getPeripheralTypeDescriptor(type);
            var identity = [
                descriptor?.id, descriptor?.key, descriptor?.slug, descriptor?.name,
                peripheral?.typeName, peripheral?.driverName
            ].filter(Boolean).join(' ').toLowerCase();
            if (type === 1 || /\buart\b/.test(identity)) return 'uart';
            if (type === 57) return 'pca9685';
            if (type === 43 || /encoder/.test(identity)) return 'encoder';
            if (type === 37 || /sdio|sd[_ -]?card|\bsd\b/.test(identity)) return 'sd';
            if (/rfid|mfrc522/.test(identity)) return 'rfid';
            if (type === 50 || /ds1302/.test(identity)) return 'ds1302';
            if (type === 52 || /lcd\s*1602|lcd1602/.test(identity)) return 'lcd1602';
            var sensorDriver = peripheral?.params?.driverName || peripheral?.params?.driver || peripheral?.driverName || peripheral?.driver;
            if (type === 2 || (type === 38 && sensorDriver) || /i2c|bmp280|mpu6050|sht3|aht20/.test(identity)) return 'i2c-sensor';
            return '';
        },

        _canOpenPeripheralDiagnostics(peripheral) {
            var descriptor = this._getPeripheralTypeDescriptor(peripheral && peripheral.type);
            // The detail/list snapshot is authoritative; catalog loading is optional.
            var implemented = typeof peripheral?.implemented === 'boolean'
                ? peripheral.implemented : descriptor?.implemented === true;
            return implemented && !!this._getPeripheralDiagnosticKind(peripheral);
        },

        _getPeripheralDiagnosticBlockReason(peripheral) {
            // A stopped/disabled PWM session must retain read-only status and
            // explicit disable/stop access; server admission guards new output.
            if (Number(peripheral?.type) === 57) return '';
            if (peripheral?.diagnosticBlockedReason === 'protocol.modbus') {
                return '此串口由 Modbus 协议管理，请到通信协议页面调试；终端收发已禁用，避免干扰总线';
            }
            if (peripheral?.enabled === false || peripheral?.diagnosticBlockedReason === 'disabled') {
                return '外设已禁用，请先在外设配置中启用后再使用诊断';
            }
            if (peripheral?.memorySuspended || peripheral?.status?.memorySuspended) return '外设因内存保护暂停，请恢复后重试';
            return '';
        },

        _diagnosticButtonLabel(kind) {
            // 诊断入口保留给接线调试，但在列表中统一成一个低优先级入口，
            // 避免“终端/探测/读数”等按钮挤占主操作区。
            return '诊断';
        },

        _diagnosticButtonTitle(kind) {
            var labels = {
                uart: '打开串口终端进行收发测试',
                encoder: '查看编码器实时读数',
                sd: '管理存储卡文件',
                rfid: '执行 RFID 读卡操作',
                ds1302: '查看并设置时钟',
                lcd1602: '测试字符屏显示',
                pca9685: '配置 PWM 通道并管理输出会话',
                'i2c-sensor': '扫描并读取 I2C 设备'
            };
            return labels[kind] || '打开外设诊断工具';
        },

        _setPeripheralCapacity(profile) {
            this._periphProfile = profile || null;
            var btn = document.getElementById('add-peripheral-btn');
            if (!btn) return;
            var locked = !!(profile && Number(profile.remaining) <= 0);
            if (locked) {
                btn.setAttribute('data-resource-locked', 'true');
                btn.disabled = true;
                btn.title = '当前资源档位外设数量已达上限';
            } else {
                btn.removeAttribute('data-resource-locked');
                if (this.isDeveloperModeEnabled()) {
                    btn.disabled = false;
                    btn.removeAttribute('title');
                }
            }
        },

        _formatPeripheralCapacitySummary(total) {
            var summary = '共 ' + total + ' 项';
            var profile = this._periphProfile;
            var max = profile && (profile.max !== undefined ? profile.max : profile.maxItems);
            if (profile && max !== undefined) {
                summary += ' · 容量 ' + (profile.used || 0) + ' / ' + max;
            }
            return summary;
        },

        _loadPeripheralSaveModule(callback) {
            var self = this;
            var run = function() {
                if (typeof callback === 'function') callback.call(self);
            };
            if (typeof this.savePeripheralConfig === 'function') {
                run();
                return;
            }
            if (typeof ModuleLoader !== 'undefined' && typeof ModuleLoader.loadModule === 'function') {
                ModuleLoader.loadModule('peripheral-save', run);
            }
        },

        _savePeripheralConfigWithModule() {
            if (typeof this.savePeripheralConfig === 'function') {
                this.savePeripheralConfig();
                return;
            }
            var saveBtn = document.getElementById('save-peripheral-btn');
            var originalText = saveBtn?.textContent || '保存';
            if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = '加载校验...'; }
            this._loadPeripheralSaveModule(function() {
                if (saveBtn) saveBtn.textContent = originalText;
                if (typeof this.savePeripheralConfig === 'function') this.savePeripheralConfig();
            });
        },

        _openPeripheralResources() {
            const page = this.currentPage;
            const token = this._periphResourceOpen = {};
            const run = () => {
                if (this._periphResourceOpen === token && this.currentPage === page && typeof this.openPeripheralResources === 'function')
                    this.openPeripheralResources();
            };
            if (typeof this.openPeripheralResources === 'function') run();
            else if (typeof ModuleLoader !== 'undefined') ModuleLoader.loadModule('peripheral-io', run);
        },

        _openPeripheralIoWorkspace(id) {
            var self = this;
            var run = function() {
                if (typeof self.openPeripheralIoModal === 'function') self.openPeripheralIoModal(id);
            };
            if (typeof this.openPeripheralIoModal === 'function') {
                run();
                return;
            }
            if (typeof ModuleLoader === 'undefined' || typeof ModuleLoader.loadModule !== 'function') {
                if (typeof Notification !== 'undefined') Notification.error('外设诊断模块加载失败，请刷新页面重试');
                return;
            }
            ModuleLoader.loadModule('peripheral-io', run);
        },

        // ============ 外设列表 ============

        _refreshPeripheralList() {
            var btn = document.getElementById('peripheral-refresh-btn');
            if (btn && btn.disabled) return;
            if (btn) { btn.disabled = true; btn.textContent = '加载中...'; }
            if (typeof window.apiInvalidateCache === 'function') {
                window.apiInvalidateCache('/api/peripherals');
            }
            var filter = document.getElementById('peripheral-filter-type');
            this.loadPeripherals((filter && filter.value) || '', { noCache: true });
            setTimeout(function() {
                if (btn) { btn.disabled = false; btn.innerHTML = '&#x21bb; 刷新'; }
            }, 2000);
        },

        loadPeripherals(filterType = '', options) {
            if (this._periphListFilter !== filterType) {
                this._periphCurrentPage = 1;
                this._periphListFilter = filterType;
            }
            const requestSequence = this._periphListSequence = (this._periphListSequence || 0) + 1;
            const tbody = document.getElementById('peripheral-table-body');
            if (!tbody) return;
            this.applyDeveloperModeState();
            // Show/hide developer mode hint banner
            var devHint = document.getElementById('peripheral-dev-mode-hint');
            if (devHint) devHint.style.display = this.isDeveloperModeEnabled() ? 'none' : 'block';
            this.renderEmptyTableRow(tbody, 6, '加载中...');
            var getter = (options && options.noCache === true && typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            var params = {
                page: this._periphCurrentPage,
                pageSize: this._periphPageSize
            };
            if (filterType) params.category = filterType;
            return getter('/api/peripherals', params)
                .then(res => {
                    if (requestSequence !== this._periphListSequence) return;
                    // List responses carry the same runtime memory-degradation
                    // snapshot as /api/system/status. Feed it to the shared
                    // governor so this page also slows polling and shows the
                    // warning when the firmware is protecting its heap.
                    if (typeof apiReportRuntimePressure === 'function') {
                        apiReportRuntimePressure(res, 'peripherals');
                    }
                    if (!res || !res.success) {
                        var detail = (res && res.message) ? ' (' + res.message + ')' : '';
                        this.renderEmptyTableRow(tbody, 6, '加载失败' + detail, 'u-empty-cell u-text-danger');
                        this._renderPeriphPagination(0, 1, 10);
                        return;
                    }
                    const total = res.total || 0;
                    const page = res.page || 1;
                    const pageSize = res.pageSize || 10;
                    const lastPage = Math.max(1, Math.ceil(total / pageSize));
                    this._periphCurrentPage = Math.min(page, lastPage);
                    if (page > lastPage) return this.loadPeripherals(filterType, { noCache: true });
                    this._periphTotalCount = total;
                    this._setPeripheralCapacity(res.profile || null);
                    const peripherals = res.data || [];
                    if (peripherals.length === 0) {
                        this.renderEmptyTableRow(tbody, 6, '暂无外设配置');
                        this._renderPeriphPagination(total, page, pageSize);
                        return;
                    }
                    let html = '';
                    peripherals.forEach(periph => {
                        const statusBadgeClasses = { 0: 'badge-info', 1: 'badge-warning', 2: 'badge-success', 3: 'badge-primary', 4: 'badge-danger' };
                        const statusNames = {
                            0: '已禁用', 1: '已启用',
                            2: '已初始化', 3: '运行中',
                            4: '错误'
                        };
                        const statusName = periph.featureActive === false ? '功能未启用' : (statusNames[periph.status] || '未知');
                        const statusBadgeClass = statusBadgeClasses[periph.status] || 'badge-info';
                        const safeId = escapeHtml(periph.id);
                        const safeName = escapeHtml(periph.name);
                        const safeType = escapeHtml(periph.typeName || periph.type || '--');
                        const typeTag = periph.implemented === false ? ' <span class="badge badge-info" style="font-size:10px">待驱动</span>' : '';
                        const ioParams = periph.params || periph;
                        const logicalIo = ioParams.ioSource === 'expander' || Number(ioParams.ioSource) === 1;
                        const pinsStr = escapeHtml(logicalIo
                            ? (ioParams.providerId || '?') + ' / P' + ioParams.channel
                            : (periph.pins && periph.pins.length ? periph.pins.join(', ') : (ioParams.busPeripheralId || '--')));
                        const diagnosticKind = this._getPeripheralDiagnosticKind(periph);
                        const diagnosticButton = this._canOpenPeripheralDiagnostics(periph)
                            ? this._renderPeripheralActionButton('diagnose', periph.id, this._diagnosticButtonLabel(diagnosticKind), 'fb-btn-secondary peripheral-diagnostic-action', false, this._diagnosticButtonTitle(diagnosticKind))
                            : '';
                        html += '<tr>' +
                            '<td>' + safeId + '</td>' +
                            '<td>' + safeName + '</td>' +
                            '<td>' + safeType + typeTag + '</td>' +
                            '<td>' + pinsStr + '</td>' +
                            '<td><span class="badge ' + statusBadgeClass + '">' + escapeHtml(statusName) + '</span></td>' +
                            '<td class="u-cell-nowrap"><div class="u-table-action-row">' +
                                this._renderPeripheralActionButton('edit', periph.id, '编辑', 'fb-btn-primary') +
                                this._renderPeripheralActionButton('toggle', periph.id, periph.enabled ? '禁用' : '启用', periph.enabled ? 'fb-btn-warning' : 'fb-btn-success') +
                                this._renderPeripheralActionButton('delete', periph.id, '删除', 'fb-btn-danger') +
                                diagnosticButton +
                            '</div></td></tr>';
                    });
                    tbody.innerHTML = html;
                    this._renderPeriphPagination(total, page, pageSize);
                    this.applyDeveloperModeState(tbody);
                })
                .catch(err => {
                    if (requestSequence !== this._periphListSequence) return;
                    // 页面切换导致的请求中止属预期行为，静默返回不渲染错误行
                    if (typeof window.isPageAbortError === 'function' && window.isPageAbortError(err)) return;
                    var msg = '加载失败';
                    if (err && err.status) msg += ' (HTTP ' + err.status + ')';
                    else if (err && err.message) msg += ' (' + err.message + ')';
                    console.error('Load peripherals failed:', err);
                    this.renderEmptyTableRow(tbody, 6, msg, 'u-empty-cell u-text-danger');
                    this._renderPeriphPagination(0, 1, 10);
                });
        },

        _renderPeriphPagination(total, page, pageSize) {
            const summaryText = this._formatPeripheralCapacitySummary(total);
            const summary = document.getElementById('peripheral-list-summary');
            if (summary) summary.textContent = summaryText;

            const pagination = document.getElementById('periph-pagination');
            const hasMultiplePages = Math.ceil(Math.max(0, Number(total) || 0) / Math.max(1, Number(pageSize) || 10)) > 1;
            if (pagination) {
                pagination.hidden = !hasMultiplePages;
                if (!hasMultiplePages) pagination.innerHTML = '';
            }
            if (!hasMultiplePages) return;

            const filterValue = document.getElementById('peripheral-filter-type')?.value || '';
            this.renderPagination('periph-pagination', {
                total,
                page,
                pageSize,
                maxVisiblePages: 3,
                onPageChange: (nextPage) => {
                    this._periphCurrentPage = nextPage;
                    this.loadPeripherals(filterValue);
                }
            });
        },

        // ============ 外设模态框 ============

        async openPeripheralModal(isEdit = false, peripheralId = null) {
            if (!this.guardDeveloperModeAction()) return;
            if (!isEdit && this._periphProfile && Number(this._periphProfile.remaining) <= 0) {
                Notification.warning(t('peri-capacity-full', '当前资源档位外设数量已达上限'), '外设配置');
                return;
            }
            const session = {};
            this._periphFormSession = session;
            this._loadPeripheralSaveModule();
            const modal = await this._ensureModalForAction('peripheral-modal');
            if (this._periphFormSession !== session) return;
            const title = document.getElementById('peripheral-modal-title');
            const form = document.getElementById('peripheral-form');
            if (!modal) {
                return;
            }
            form?.reset();
            this._restoreRtcBootSeed({});
            this._periphFormConfig = null;
            this._pendingSensorDriverParams = null;
            this._loadPeripheralProviders();
            this.clearInlineError('peripheral-error');
            this._resetPeripheralPreflight();
            document.querySelectorAll('.peripheral-params-group').forEach(el => this.hideElement(el));
            if (!Array.isArray(this._periphTypeCatalog) && !this._periphTypeCatalogLoading) {
                this._loadPeripheralHardwareCapabilities();
            }
            this._syncPeripheralTypeSelect(this._periphTypeCatalog);
            if (isEdit && peripheralId) {
                title.textContent = t('peri-modal-title-edit', '编辑外设');
                this.loadPeripheralForEdit(peripheralId);
            } else {
                title.textContent = t('peri-modal-title-add', '新增外设');
                document.getElementById('peripheral-original-id').value = '';
                document.getElementById('peripheral-id-input').disabled = false;
                // 新增外设默认禁用（向导式：先保存再测试再启用）
                var enabledCb = document.getElementById('peripheral-enabled-input');
                if (enabledCb) enabledCb.checked = false;
                // 根据 form.reset() 后的实际下拉值初始化参数区域，避免类型与参数不匹配
                var typeSelect = document.getElementById('peripheral-type-input');
                this.onPeripheralTypeChange(typeSelect ? typeSelect.value : '1');
            }
            this.showModal(modal);
        },

        closePeripheralModal() {
            this._periphFormSession = null;
            // 关闭时销毁红外管理面板（停止学习轮询）
            if (typeof this.teardownIrManage === 'function') this.teardownIrManage();
            // 关闭时销毁 RF 管理面板
            if (typeof this.teardownRfManage === 'function') this.teardownRfManage();
            this._resetPeripheralPreflight();
            this.hideModal('peripheral-modal');
            this.hideModal('gpio-modal');
        },

        onPeripheralTypeChange(typeValue) {
            const type = parseInt(typeValue);
            this._updatePeripheralTypeAvailability(type);
            document.querySelectorAll('.peripheral-params-group').forEach(el => this.hideElement(el));
            // 类型切离 53 时销毁红外管理面板（停止学习轮询）
            if (type !== 53 && typeof this.teardownIrManage === 'function') this.teardownIrManage();
            // 类型切离 48 时销毁 RF 管理面板
            if (type !== 48 && typeof this.teardownRfManage === 'function') this.teardownRfManage();
            const pinHint = document.getElementById('uart-pin-direction-hint');
            this.hideElement(pinHint);

            // 引脚推荐提示
            var pinRecommendEl = document.getElementById('peripheral-pins-recommend');
            if (pinRecommendEl) {
                var rec = this._getPinRecommendation(type);
                pinRecommendEl.textContent = rec;
                pinRecommendEl.style.display = rec ? '' : 'none';
            }

            // GPIO_ANALOG_INPUT (15) uses the ADC parameter layout. Keep it
            // out of the generic GPIO form so AI resolution/filter settings
            // are visible and submitted consistently with type 26.
            if (type >= 11 && type <= 21 && type !== 15) {
                const gpioParams = document.getElementById('gpio-params');
                if (gpioParams) {
                    this.showElement(gpioParams);
                    gpioParams.querySelectorAll('.pwm-only').forEach(el => {
                        (type === 17 || type === 16) ? this.showElement(el) : this.hideElement(el);
                    });
                    // AO 模拟量输出选项：仅 PWM/模拟输出类型可见
                    gpioParams.querySelectorAll('.ao-only').forEach(el => {
                        (type === 17 || type === 16) ? this.showElement(el) : this.hideElement(el);
                    });
                     gpioParams.querySelectorAll('.input-only').forEach(el => {
                         (type === 11 || type === 13 || type === 14 || type === 18 || type === 19 || type === 20)
                             ? this.showElement(el) : this.hideElement(el);
                     });
                }
            } else if (type === 26 || type === 15) {
                this.showElement(document.getElementById('adc-params'));
            } else {
                const group = {1: 'uart', 2: 'i2c', 3: 'spi', 27: 'dac', 36: 'lcd', 37: 'sdcard',
                    38: 'generic-sensor', 41: 'servo', 42: 'stepper', 43: 'encoder', 45: 'neopixel',
                    47: 'segment', 48: 'rf', 49: 'radar', 50: 'ds1302', 52: 'lcd1602',
                    55: 'io-expander', 56: 'display', 57: 'pca9685'}[type];
                if (group) this.showElement(document.getElementById(group + '-params'));
            }
            if (type === 1) this.showElement(pinHint);
            else if (type === 38) this._updateGenericSensorDriverFields();
            else if (type === 55) this._updateIoExpanderFields(false);
            else if (type === 56) this._updateDisplayFields();
            else if (type === 57) this._updatePca9685Fields();
            else if (type === 37) this._updateSdcardPinHints();
            else if (type === 48) this._showPeripheralRemoteManagement('rf', '_initRfManagePanel');
            else if (type === 53) this._showPeripheralRemoteManagement('ir', '_initIrManagePanel');
            // DISPLAY (56) 使用具名引脚字段；DEVICE_EVENT (60) 和 Modbus (51) 无通用引脚配置。
            const pinsGroup = document.getElementById('peripheral-pins-group');
            if (pinsGroup) {
                (type === 60 || type === 51 || type === 56 || type === 57) ? this.hideElement(pinsGroup) : this.showElement(pinsGroup);
            }
            
            for (const [value, name] of [[60, 'device-event'], [38, 'generic-sensor'],
                [50, 'ds1302'], [52, 'lcd1602']]) {
                const hint = document.getElementById(name + '-hint');
                (type === value) ? this.showElement(hint) : this.hideElement(hint);
            }
            this._updateGpioSourceFields();
            this._updatePeripheralPinRoleHint(type);
        },

        _showPeripheralRemoteManagement(kind, initialize) {
            const section = document.getElementById(kind + '-manage-section');
            if (!section) return;
            this.showElement(section);
            const originalId = document.getElementById('peripheral-original-id')?.value || '';
            const hintNew = document.getElementById(kind + '-manage-hint-new');
            const content = document.getElementById(kind + '-manage-content');
            if (hintNew) hintNew.style.display = originalId ? 'none' : '';
            if (content) content.style.display = originalId ? '' : 'none';
            if (originalId) this[initialize]();
        },

        loadPeripheralForEdit(id) {
            const session = this._periphFormSession;
            var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            return getter('/api/peripherals/', { id: id })
                .then(res => {
                    if (this._periphFormSession !== session) return;
                    if (res && res.success && res.data) {
                        const data = res.data;
                        this._periphFormConfig = data;
                        const safeValue = (val, def = '') => (val !== undefined && val !== null) ? val : def;
                        document.getElementById('peripheral-original-id').value = safeValue(data.id);
                        document.getElementById('peripheral-id-input').value = safeValue(data.id);
                        document.getElementById('peripheral-id-input').disabled = true;
                        document.getElementById('peripheral-name-input').value = safeValue(data.name);
                        document.getElementById('peripheral-type-input').value = safeValue(data.type, '11');
                        this._syncPeripheralTypeSelect(this._periphTypeCatalog);
                        document.getElementById('peripheral-enabled-input').checked = data.enabled ? true : false;
                        if (data.pins && Array.isArray(data.pins)) {
                            const validPins = data.pins.filter(p => p !== 255 && p !== undefined && p !== null);
                            document.getElementById('peripheral-pins-input').value = validPins.join(',');
                        } else {
                            document.getElementById('peripheral-pins-input').value = '';
                        }
                        this.onPeripheralTypeChange(data.type);
                        if (data.params) {
                            [
                                ['gpio-initial-state', 'initialState'],
                                ['gpio-pwm-freq', 'pwmFrequency'],
                                ['gpio-pwm-resolution', 'pwmResolution'],
                                ['uart-baudrate', 'baudRate'],
                                ['uart-databits', 'dataBits'],
                                ['uart-stopbits', 'stopBits'],
                                ['uart-parity', 'parity'],
                                ['i2c-address', 'address'],
                                ['spi-mode', 'mode'],
                                ['adc-resolution', 'resolution'],
                                ['adc-attenuation', 'attenuation']
                            ].forEach(([id, key]) => {
                                if (data.params[key] !== undefined)
                                    document.getElementById(id).value = data.params[key];
                            });
                            if (data.params.frequency !== undefined) {
                                var i2cFreq = document.getElementById('i2c-frequency');
                                if (i2cFreq) i2cFreq.value = data.params.frequency;
                                var spiFreq = document.getElementById('spi-frequency');
                                if (spiFreq) spiFreq.value = data.params.frequency;
                            }
                            // Scalar text fields preserve defaults when older configurations omit them.
                            [
                                ['adc-sample-rate', 'sampleRate'],
                                ['adc-sample-count', 'sampleCount'],
                                ['adc-filter-mode', 'filterMode'],
                                ['generic-sensor-type', 'sensorType'],
                                ['generic-sensor-interval', 'sampleInterval'],
                                ['generic-sensor-bus-id', 'busPeripheralId'],
                                ['adc-signal-mode', 'signalMode'],
                                ['adc-input-min', 'inputMin'],
                                ['adc-input-max', 'inputMax'],
                                ['adc-engineering-min', 'engineeringMin'],
                                ['adc-engineering-max', 'engineeringMax'],
                                ['adc-engineering-offset', 'engineeringOffset'],
                                ['adc-decimal-places', 'decimalPlaces'],
                                ['adc-vref', 'vRef'],
                                ['adc-voltage-ratio', 'voltageRatio'],
                                ['adc-shunt-resistance', 'shuntResistance'],
                                ['adc-input-gain', 'inputGain'],
                                ['adc-input-offset', 'inputOffset'],
                                ['adc-raw-min', 'rawMin'],
                                ['adc-raw-max', 'rawMax']
                            ].forEach(function (mapping) {
                                const el = document.getElementById(mapping[0]);
                                if (el && data.params[mapping[1]] !== undefined) el.value = String(data.params[mapping[1]]);
                            });
                            { const el = document.getElementById('adc-conversion-enabled'); if (el && data.params.conversionEnabled !== undefined) el.checked = !!data.params.conversionEnabled; }
                            { const el = document.getElementById('adc-clamp-enabled'); if (el && data.params.clampEnabled !== undefined) el.checked = !!data.params.clampEnabled; }
                            { const el = document.getElementById('adc-calibrated-voltage'); if (el) el.checked = !!data.params.calibratedVoltage; }
                            // 通用传感器参数
                            {
                                const el = document.getElementById('generic-sensor-driver');
                                const driver = data.params.driverName || data.params.driver;
                                if (el && driver !== undefined) el.value = String(driver).toLowerCase();
                            }
                            {
                                const el = document.getElementById('generic-sensor-address');
                                const address = data.params.i2cAddress ?? data.params.address;
                                if (el && address !== undefined) el.value = this._formatI2cAddress(address);
                            }
                            // AO 模拟量输出模式回显
                            { const el = document.getElementById('gpio-ao-mode'); if (el) el.checked = !!data.params.aoMode; }
                            { const el = document.getElementById('gpio-ao-min-pct'); if (el && data.params.aoMinPct !== undefined) el.value = data.params.aoMinPct; }
                            { const el = document.getElementById('gpio-ao-max-pct'); if (el && data.params.aoMaxPct !== undefined) el.value = data.params.aoMaxPct; }
                            if (data.params.defaultDuty !== undefined) { const el = document.getElementById('gpio-default-duty'); if (el) el.value = data.params.defaultDuty; }
                            if (data.params.debounceMs !== undefined) { const el = document.getElementById('gpio-debounce-ms'); if (el) el.value = data.params.debounceMs; }
                            if (data.params.inputMode !== undefined) { const el = document.getElementById('gpio-input-mode'); if (el) el.value = String(data.params.inputMode); }
                            { const el = document.getElementById('gpio-invert-logic'); if (el && data.params.invertLogic !== undefined) el.checked = !!data.params.invertLogic; }
                            if (data.params.reportMode !== undefined) { const el = document.getElementById('gpio-report-mode'); if (el) el.value = String(data.params.reportMode); }
                            { const el = document.getElementById('gpio-paired-input-id'); if (el) el.value = data.params.pairedInputId || ''; }
                            { const el = document.getElementById('gpio-pair-debounce-ms'); if (el) el.value = data.params.pairDebounceMs || 200; }
                            if (data.params.defaultValue !== undefined) { const el = document.getElementById('dac-default-value'); if (el) el.value = data.params.defaultValue; }
                            if (Number(data.type) === 41) {
                                [
                                    ['servo-frequency', 'frequency'],
                                    ['servo-min-pulse', 'minPulse'],
                                    ['servo-max-pulse', 'maxPulse'],
                                    ['servo-min-angle', 'minAngle'],
                                    ['servo-max-angle', 'maxAngle'],
                                    ['servo-initial-angle', 'initialAngle']
                                ].forEach(function (mapping) {
                                    const el = document.getElementById(mapping[0]);
                                    if (el && data.params[mapping[1]] !== undefined) el.value = String(data.params[mapping[1]]);
                                });
                                { const el = document.getElementById('servo-inverted'); if (el) el.checked = !!data.params.inverted; }
                                { const el = document.getElementById('servo-release-after-move'); if (el) el.checked = !!data.params.releaseAfterMove; }
                            }
                            // All fields retain an existing/default value when an old
                            // configuration omits the parameter. DOM value coercion is unchanged.
                            [
                                ['segment-brightness', 'brightness'],
                                ['stepper-steps-per-rev', 'stepsPerRevolution'],
                                ['stepper-speed', 'speed'],
                                ['neopixel-count', 'count'],
                                ['neopixel-brightness', 'brightness'],
                                ['rf-mode', 'mode'],
                                ['rf-bit-length', 'bitLength'],
                                ['rf-pulse-width', 'pulseWidth'],
                                ['rf-repeat', 'repeat']
                            ].forEach(([id, key]) => {
                                const input = document.getElementById(id);
                                if (input && data.params[key] !== undefined) input.value = data.params[key];
                            });
                            if (data.params.activeHigh !== undefined) {
                                const rfActive = document.getElementById('rf-active-high');
                                const radarActive = document.getElementById('radar-active-high');
                                if (rfActive) rfActive.checked = !!data.params.activeHigh;
                                if (radarActive) radarActive.checked = !!data.params.activeHigh;
                            }
                            [
                                ['radar-debounce-ms', 'debounceMs'],
                                ['radar-hold-ms', 'holdMs'],
                                ['lcd-width', 'width'],
                                ['lcd-height', 'height'],
                                ['lcd-interface', 'interface'],
                                ['lcd-contrast', 'contrast'],
                                ['encoder-resolution', 'resolution']
                            ].forEach(([id, key]) => {
                                const input = document.getElementById(id);
                                if (input && data.params[key] !== undefined) input.value = data.params[key];
                            });
                            if (data.params.useInterrupt !== undefined) { const el = document.getElementById('encoder-use-interrupt'); if (el) el.checked = !!data.params.useInterrupt; }
                            // SD卡参数回填
                            {
                                const params = data.params.sdcard || data.params;
                                if (Number(data.type) === 37 && params.interface !== undefined) {
                                    const el = document.getElementById('sdcard-interface');
                                    if (el) el.value = String(params.interface);
                                }
                                if (Number(data.type) === 37 && params.frequency !== undefined) {
                                    const el = document.getElementById('sdcard-frequency');
                                    if (el) el.value = String(params.frequency);
                                }
                            }
                            if (Number(data.type) === 50) {
                                this._restoreRtcBootSeed(data.params);
                                { const el = document.getElementById('ds1302-hour-mode'); if (el && data.params.use24Hour !== undefined) el.value = data.params.use24Hour ? '1' : '0'; }
                                { const el = document.getElementById('ds1302-trickle-charge'); if (el && data.params.trickleCharge !== undefined) el.value = String(data.params.trickleCharge); }
                            }
                            if (Number(data.type) === 52) {
                                { const el = document.getElementById('lcd1602-size'); if (el && data.params.cols && data.params.rows) el.value = data.params.cols + 'x' + data.params.rows; }
                                { const el = document.getElementById('lcd1602-address'); if (el && data.params.address !== undefined) el.value = this._formatI2cAddress(data.params.address); }
                                { const el = document.getElementById('lcd1602-backlight'); if (el && data.params.backlight !== undefined) el.checked = !!data.params.backlight; }
                            }
                            if (Number(data.type) === 38) {
                                this._updateGenericSensorDriverFields(false, data.params);
                            } else if (Number(data.type) === 55) {
                                this._updateIoExpanderFields(false, data.params);
                            } else if (Number(data.type) === 57) {
                                this._updatePca9685Fields(data.params);
                            } else if (Number(data.type) === 56) {
                                this._updateDisplayFields(data.params);
                            }
                            if ([11, 12, 13, 14, 18, 19, 20].indexOf(Number(data.type)) >= 0) {
                                const params = data.params.gpio || data.params;
                                document.getElementById('gpio-io-source').value = params.ioSource === 'expander' || Number(params.ioSource) === 1 ? 'expander' : 'gpio';
                                this._setPeripheralReferenceOptions('gpio-provider-id', 55, params.providerId || '');
                                this._updateGpioSourceFields(params.channel ?? 0);
                            }
                            this._updateSdcardPinHints();
                        }
                    } else {
                        Notification.error(t('peri-load-fail', '加载失败'), '外设配置');
                    }
                })
                .catch(err => {
                    if (this._periphFormSession !== session) return;
                    console.error('Load peripheral for edit failed:', err);
                    Notification.error(t('peri-load-fail', '加载失败'), '外设配置');
                });
        },

        _loadPeripheralHardwareCapabilities() {
            if (this._periphTypeCatalogLoading) return;
            this._periphTypeCatalogLoading = true;
            this._periphHardware = null;
            this._periphTypeCatalog = null;
            this._syncPeripheralTypeSelect(null);
            apiGet('/api/peripherals/types').then((res) => {
                const groups = (res && res.success && res.data && typeof res.data === 'object') ? res.data : null;
                const catalog = groups ? Object.keys(groups).reduce(function (items, key) {
                    var group = Array.isArray(groups[key]) ? groups[key].map(function(item) {
                        return Object.assign({ _category: key }, item);
                    }) : [];
                    return items.concat(group);
                }, []) : [];
                if (!res || !res.success || !res.hardware || catalog.length === 0) {
                    throw new Error('Invalid peripheral capability response');
                }
                this._periphHardware = res.hardware;
                this._periphTypeCatalog = catalog;
                this._syncPeripheralTypeSelect(catalog);
            }).catch((err) => {
                this._periphHardware = null;
                this._periphTypeCatalog = null;
                this._syncPeripheralTypeSelect(null);
                console.warn('Load peripheral hardware capabilities failed:', err);
            }).finally(() => {
                this._periphTypeCatalogLoading = false;
            });
        },

        _isPeripheralTypeAvailable(typeValue) {
            const type = Number(typeValue);
            if (!Array.isArray(this._periphTypeCatalog) || !Number.isFinite(type)) return false;
            const item = this._periphTypeCatalog.find(entry => Number(entry && (entry.value ?? entry.type)) === type);
            return !!item && item.configurable !== false && item.implemented !== false;
        },

        _updatePeripheralTypeAvailability(typeValue) {
            const ready = Array.isArray(this._periphTypeCatalog) && this._periphTypeCatalog.length > 0;
            const available = ready && this._isPeripheralTypeAvailable(typeValue);
            const typeSelect = document.getElementById('peripheral-type-input');
            const saveBtn = document.getElementById('save-peripheral-btn');
            const status = document.getElementById('peripheral-type-status');
            if (typeSelect) typeSelect.disabled = !ready;
            if (saveBtn) saveBtn.disabled = !available;
            if (status) {
                status.textContent = !ready
                    ? t('peri-types-unavailable', '设备外设能力尚未加载，请稍后重试')
                    : (!available ? t('peri-type-unavailable', '该外设类型当前不可用') : '');
                status.style.display = available ? 'none' : '';
            }
            return available;
        },

        _syncPeripheralTypeSelect(catalog) {
            const typeSelect = document.getElementById('peripheral-type-input');
            if (!typeSelect) return;
            const selectedValue = typeSelect.value;
            const ready = Array.isArray(catalog) && catalog.length > 0;
            const byValue = Object.create(null);
            if (ready) {
                catalog.forEach(function (item) {
                    var value = item && (item.value ?? item.type);
                    if (value !== undefined) byValue[String(value)] = item;
                });
                var dynamicGroup = typeSelect.querySelector('optgroup[data-descriptor-types="true"]');
                if (dynamicGroup) {
                    dynamicGroup.remove();
                    dynamicGroup = null;
                }
                catalog.forEach(function(item) {
                    var value = item && (item.value ?? item.type);
                    if (value === undefined || !Number.isFinite(Number(value)) || item.configurable === false) return;
                    var existing = Array.from(typeSelect.options).find(function(option) {
                        return String(option.value) === String(value);
                    });
                    if (existing) return;
                    if (!dynamicGroup) {
                        dynamicGroup = document.createElement('optgroup');
                        dynamicGroup.label = '设备声明';
                        dynamicGroup.dataset.descriptorTypes = 'true';
                        typeSelect.appendChild(dynamicGroup);
                    }
                    var option = document.createElement('option');
                    option.value = String(value);
                    option.dataset.dynamicDescriptor = 'true';
                    dynamicGroup.appendChild(option);
                });
                if (selectedValue && Array.from(typeSelect.options).some(function(option) {
                    return option.value === selectedValue;
                })) {
                    typeSelect.value = selectedValue;
                }
            }
            var localNames = {
                50: 'DS1302 实时时钟',
                52: 'LCD1602 字符液晶',
                54: 'MFRC522 RFID',
                56: '彩色显示与触摸',
                57: 'PCA9685 PWM'
            };
            Array.from(typeSelect.options).forEach(function (option) {
                const item = ready ? byValue[String(option.value)] : null;
                const configurable = !!item && item.configurable !== false;
                const implemented = configurable && item.implemented !== false;
                option.disabled = !implemented;
                option.hidden = !!item && !configurable;
                option.dataset.implemented = implemented ? 'true' : (item ? 'false' : 'unknown');
                if (item && (option.dataset.dynamicDescriptor === 'true' || localNames[Number(option.value)])) {
                    option.textContent = item.displayName || item.label || localNames[Number(option.value)] || item.name || ('类型 ' + option.value);
                }
                option.title = item && !configurable
                    ? '该类型由其他功能页面管理'
                    : (item && !implemented ? '当前固件未实现该外设类型' : '');
            });

            const originalId = document.getElementById('peripheral-original-id')?.value || '';
            const selected = typeSelect.options[typeSelect.selectedIndex];
            if (ready && !originalId && (!selected || selected.disabled)) {
                const firstAvailable = Array.from(typeSelect.options).find(option => !option.disabled && !option.hidden);
                if (firstAvailable) typeSelect.value = firstAvailable.value;
            }
            this._updatePeripheralTypeAvailability(typeSelect.value);
            if (Number(typeSelect.value) === 38) this._updateGenericSensorDriverFields(false);
            if (Number(typeSelect.value) === 55) this._updateIoExpanderFields(false);
            if (Number(typeSelect.value) === 56) this._updateDisplayFields();
            if (Number(typeSelect.value) === 57) this._updatePca9685Fields();
            if (Number(typeSelect.value) === 37) this._updateSdcardPinHints();
        },

        _descriptorPinRoles(type) {
            var descriptor = this._getPeripheralTypeDescriptor(type);
            var roles = descriptor && (descriptor.pinRoles || descriptor.roles);
            if (roles && !Array.isArray(roles) && typeof roles === 'object') {
                var variant = type === 37 && document.getElementById('sdcard-interface')?.value === '0'
                    ? 'sdmmc' : (type === 37 ? 'spi' : 'default');
                roles = roles[variant] || roles.default || roles.pins;
            }
            if (!Array.isArray(roles)) return [];
            return roles.map(function(role) {
                if (typeof role === 'string') return role;
                return role && (role.label || role.name || role.role || role.id);
            }).filter(Boolean);
        },

        _fallbackPinRoles(type) {
            var roles = {
                1: ['RX', 'TX'],
                2: ['SDA', 'SCL'],
                3: ['MISO', 'MOSI', 'SCK', 'CS'],
                37: document.getElementById('sdcard-interface')?.value === '0'
                    ? ['CMD', 'CLK', 'D0', 'D1', 'D2', 'D3'] : ['SCK', 'MOSI', 'MISO', 'CS'],
                38: ['SDA', 'SCL'],
                41: ['PWM'],
                42: ['IN1', 'IN2', 'IN3', 'IN4'],
                43: ['A', 'B'],
                44: ['DATA'],
                45: ['DATA'],
                47: ['CLK', 'DIO'],
                48: ['DATA'],
                49: ['OUT'],
                50: ['CE', 'IO', 'SCLK'],
                52: ['SDA', 'SCL'],
                53: ['DATA'],
                54: ['SCK', 'MOSI', 'MISO', 'SS', 'RST']
            };
            return roles[type] || [];
        },

        _updatePeripheralPinRoleHint(typeValue) {
            var type = Number(typeValue);
            var hint = document.getElementById('peripheral-pin-role-hint');
            var input = document.getElementById('peripheral-pins-input');
            if (!hint || !input) return;
            const source = document.getElementById('gpio-io-source')?.value;
            const logical = [11, 12, 13, 14, 18, 19, 20].indexOf(type) >= 0 && source === 'expander';
            const bus = type === 38 ? document.getElementById('generic-sensor-bus-id')?.value
                : (type === 55 ? document.getElementById('io-expander-bus-id')?.value : '');
            const namedPins = type === 56 || type === 57;
            input.disabled = logical || !!bus || namedPins;
            input.required = !logical && !bus && !namedPins && type !== 38 && type !== 60 && type !== 51;
            const group = document.getElementById('peripheral-pins-group');
            if (group) group.classList.toggle('is-hidden', logical || !!bus || namedPins || type === 60 || type === 51);
            if (namedPins) {
                hint.textContent = type === 57 ? '使用所选总线和 OE 输出外设的接线。' : '显示与触摸引脚在下方按角色配置。';
                return;
            }
            if (logical || bus) {
                hint.textContent = logical ? '逻辑通道使用父扩展器的接线。' : '使用所选 I2C 总线的接线。';
                return;
            }
            if (type === 38 && !(document.getElementById('generic-sensor-driver')?.value || '')) {
                hint.textContent = '逻辑数据源不需要物理引脚；选择本地驱动后按 SDA、SCL 填写。';
                input.placeholder = '逻辑数据源可留空';
                return;
            }
            var roles = this._descriptorPinRoles(type);
            if (roles.length === 0) roles = this._fallbackPinRoles(type);
            hint.textContent = roles.length
                ? '引脚顺序: ' + roles.map(function(role, index) { return (index + 1) + '=' + role; }).join(', ')
                : '';
            input.placeholder = roles.length ? roles.join(', ') + ' 对应的 GPIO（逗号分隔）' : '例如: 2,4,5 (逗号分隔)';
        },

        _descriptorStringList(descriptor, keys) {
            for (var i = 0; i < keys.length; i += 1) {
                var parts = keys[i].split('.');
                var value = descriptor;
                for (var p = 0; p < parts.length && value; p += 1) value = value[parts[p]];
                if (!Array.isArray(value)) continue;
                return value.map(function(item) {
                    return String(typeof item === 'string' ? item : (item.id || item.name || item.value || '')).toLowerCase();
                }).filter(Boolean);
            }
            return [];
        },

        _peripheralIoErrorMessage(err, fallback) {
            return err?.data?.message || err?.data?.error || err?.message || fallback || '操作失败';
        },

        _formatI2cAddress(value) {
            if (value === undefined || value === null || value === '') return '--';
            var number = typeof value === 'string' && /^0x/i.test(value)
                ? parseInt(value.slice(2), 16) : Number(value);
            if (!Number.isFinite(number)) return String(value);
            return '0x' + Math.max(0, Math.min(255, Math.trunc(number))).toString(16).toUpperCase().padStart(2, '0');
        },

        _getPeripheralDriverDescriptors(type) {
            const descriptor = this._getPeripheralTypeDescriptor(type);
            if (!descriptor) return [];
            if (Array.isArray(descriptor.driverDescriptors)) return descriptor.driverDescriptors;
            // Older firmware describes only driver names. Retain those drivers
            // without advertising a new driver the device never declared.
            const legacy = {
                aht20: [0x38, [['temperature', '°C'], ['humidity', '%']]],
                sht30: [0x44, [['temperature', '°C'], ['humidity', '%']]],
                sht31: [0x44, [['temperature', '°C'], ['humidity', '%']]],
                bh1750: [0x23, [['illuminance', 'lx']]],
                bmp280: [0x76, [['temperature', '°C'], ['pressure', 'hPa'], ['altitude', 'm']]],
                mpu6050: [0x68, [['accelX', 'm/s2'], ['accelY', 'm/s2'], ['accelZ', 'm/s2'], ['temperature', '°C']]]
            };
            return this._descriptorStringList(descriptor, ['drivers', 'supportedDrivers', 'capabilities.drivers', 'operations.drivers']).map(function(name) {
                const values = legacy[name] || [0, []];
                return { name: name.toUpperCase(), defaultAddress: values[0], channels: values[1].map((c) => ({ name: c[0], unit: c[1] })), params: [] };
            });
        },

        _getPeripheralDriverMeta(type, name) {
            return this._getPeripheralDriverDescriptors(type).find((driver) => String(driver.name || '').toLowerCase() === String(name || '').toLowerCase()) || null;
        },

        _fillDriverSelect(select, type, value, allowLogical) {
            const descriptor = this._getPeripheralTypeDescriptor(type);
            const selected = String(value || '').toLowerCase();
            select.innerHTML = allowLogical ? '<option value="">逻辑数据源（不初始化驱动）</option>' : '';
            this._getPeripheralDriverDescriptors(type).forEach(function(driver) {
                const option = document.createElement('option');
                option.value = String(driver.name).toLowerCase();
                option.textContent = driver.label || driver.name;
                option.disabled = !descriptor || descriptor.implemented !== true || driver.compiled === false;
                select.appendChild(option);
            });
            if (selected && !Array.from(select.options).some((o) => o.value === selected)) {
                const unavailable = document.createElement('option');
                unavailable.value = selected;
                unavailable.textContent = selected.toUpperCase() + '（当前固件未提供）';
                unavailable.disabled = true;
                select.appendChild(unavailable);
            }
            if (selected) select.value = selected;
        },

        _setFeatureAvailabilityHint(id, descriptor, featureName) {
            const element = document.getElementById(id);
            if (!element) return;
            if (!descriptor) element.textContent = '设备能力尚未加载，请重试。';
            else if (!featureName) element.textContent = '逻辑数据源无需本地驱动。';
            else if (descriptor.featureActive === false) element.textContent = (descriptor.featureReason || '功能尚未启用') + '；可保留配置，请在“功能配置”启用“' + featureName + '”并重启。';
            else element.textContent = '按已配置外设初始化；读取状态以设备诊断结果为准。';
        },

        _renderDriverParams(meta, saved, reset) {
            const host = document.getElementById('generic-sensor-driver-params');
            if (!host) return;
            const previous = {};
            host.querySelectorAll('[data-driver-param]').forEach((input) => { previous[input.dataset.driverParam] = input.value; });
            host.textContent = '';
            const refreshScan = () => this._refreshAdsScanFields(meta);
            const labels = { shuntResistance: '分流电阻', maxCurrent: '预期最大电流', conversionTimeUs: '转换时间', averaging: '平均次数', mux: '单路输入组合', scanMask: '轮询输入组合（最多 4 路）', fullScaleMv: 'ADC 满量程', dataRate: '转换速率', calibrationGain: '电压校准增益', calibrationOffset: '电压校准偏移' };
            (meta?.params || []).forEach(function(param) {
                if (param.key === 'mode' && String(meta.name).toUpperCase() === 'MPU6050') return;
                if (!/^[A-Za-z][A-Za-z0-9_]{0,39}$/.test(param.key)) return;
                const group = document.createElement('div');
                group.className = 'fb-form-group';
                const label = document.createElement('label');
                label.htmlFor = 'generic-driver-' + param.key;
                label.textContent = (param.label || labels[param.key] || param.key) + (param.unit ? ' (' + param.unit + ')' : '');
                const choices = param.choices || param.values;
                const input = document.createElement(Array.isArray(choices) ? 'select' : 'input');
                input.id = label.htmlFor;
                input.dataset.driverParam = param.key;
                if (Array.isArray(choices)) {
                    choices.forEach(function(choice) {
                        const option = document.createElement('option');
                        option.value = String(typeof choice === 'object' ? choice.value : choice);
                        option.textContent = String(typeof choice === 'object' ? (choice.label || choice.value) : choice);
                        if (String(meta.name).toUpperCase() === 'ADS1115' && param.key === 'mux') {
                            const inputs = ['AIN0 - AIN1', 'AIN0 - AIN3', 'AIN1 - AIN3', 'AIN2 - AIN3', 'AIN0 - GND', 'AIN1 - GND', 'AIN2 - GND', 'AIN3 - GND'];
                            option.textContent = inputs[Number(option.value)] || option.textContent;
                        }
                        input.appendChild(option);
                    });
                } else {
                    input.type = 'number';
                    input.step = param.type === 'integer' || param.type === 'uint' || param.type === 'int' ? '1' : 'any';
                    if (param.min !== undefined) input.min = param.min;
                    if (param.max !== undefined) input.max = param.max;
                    input.required = true;
                }
                input.value = String(saved?.[param.key] ?? (!reset ? previous[param.key] : undefined) ?? param.default ?? '');
                group.append(label, input);
                if (String(meta.name).toUpperCase() === 'ADS1115') {
                    if (param.key === 'scanMask') {
                        input.type = 'hidden';
                        const names = ['AIN0 - AIN1', 'AIN0 - AIN3', 'AIN1 - AIN3', 'AIN2 - AIN3', 'AIN0 - GND', 'AIN1 - GND', 'AIN2 - GND', 'AIN3 - GND'];
                        names.forEach(function(name, mux) {
                            const choice = document.createElement('label');
                            const check = document.createElement('input');
                            check.type = 'checkbox';
                            check.dataset.adsMux = String(mux);
                            check.checked = !!(Number(input.value) & (1 << mux));
                            check.addEventListener('change', function() {
                                input.value = String(Array.from(group.querySelectorAll('[data-ads-mux]:checked')).reduce((mask, node) => mask | (1 << Number(node.dataset.adsMux)), 0));
                                refreshScan();
                            });
                            choice.append(check, document.createTextNode(' ' + name));
                            group.appendChild(choice);
                        });
                    }
                    input.addEventListener('change', refreshScan);
                }
                host.appendChild(group);
            });
            if (String(meta?.name).toUpperCase() === 'ADS1115') {
                const note = document.createElement('p');
                note.className = 'form-help';
                note.style.gridColumn = '1 / -1';
                note.textContent = '不勾选时保留单路电压/原始码；勾选 1～4 路时按输入编号依次转换，全部成功后发布各路电压。任一路失败整轮无效；各路并非同时采样。满量程仅用于换算，不代表允许输入电压。接线须符合供电和输入额定范围；增益/偏移为共用 ADC 校准，各路工程换算在采集动作设置。';
                host.appendChild(note);
                refreshScan();
            }
        },

        _setPeripheralReferenceOptions(id, type, selectedValue) {
            const select = document.getElementById(id);
            if (!select) return;
            const selected = selectedValue === undefined ? select.value : selectedValue;
            select.innerHTML = type === 2 ? '<option value="">直接配置 SDA、SCL</option>' : type === 12 ? '<option value="">请选择专用原生输出</option>' : '<option value="">请选择已配置的扩展器</option>';
            (this._periphProviders || []).filter((p) => Number(p.type) === type &&
                (type !== 12 || p.params?.nativeControlEligible === true)).forEach(function(periph) {
                const option = document.createElement('option');
                option.value = periph.id;
                option.textContent = (periph.name || periph.id) + ' (' + periph.id + ')' + (!periph.enabled ? ' · 已禁用' : (periph.featureActive === false ? ' · 功能未启用' : ''));
                select.appendChild(option);
            });
            if (selected && !Array.from(select.options).some((o) => o.value === selected)) {
                const missing = document.createElement('option');
                missing.value = selected;
                missing.textContent = selected + '（引用待校验）';
                select.appendChild(missing);
            }
            select.value = selected || '';
        },

        async _loadPeripheralProviders() {
            const session = this._periphFormSession;
            try {
                const all = [];
                for (let page = 1; page <= 4; page++) {
                    const res = await apiGet('/api/peripherals', { page: page, pageSize: 100, compact: 1 });
                    if (this._periphFormSession !== session) return;
                    if (!res?.success || !Array.isArray(res.data)) throw new Error('引用外设加载失败');
                    all.push(...res.data);
                    if (all.length >= Number(res.total || res.data.length) || !res.data.length) break;
                }
                this._periphProviders = all;
                this._setPeripheralReferenceOptions('generic-sensor-bus-id', 2);
                this._setPeripheralReferenceOptions('io-expander-bus-id', 2);
                this._setPeripheralReferenceOptions('io-expander-reset-id', 12);
                this._setPeripheralReferenceOptions('pca9685-bus-id', 2);
                this._setPeripheralReferenceOptions('pca9685-oe-id', 12);
                this._setPeripheralReferenceOptions('gpio-provider-id', 55);
                this._updateGpioSourceFields();
            } catch (err) {
                if (this._periphFormSession !== session) return;
                const status = document.getElementById('gpio-source-status');
                if (status) status.textContent = '引用外设加载失败，请重新打开配置。';
            }
        },

        _updateDisplayFields(saved) {
            var params = saved && typeof saved === 'object' ? saved : null;
            var pins = params && params.pins && typeof params.pins === 'object' ? params.pins : {};
            var descriptor = this._getPeripheralTypeDescriptor(56) || {};
            var limits = descriptor.configLimits || {};
            var setBounds = function(id, min, max) {
                var input = document.getElementById(id);
                if (!input) return;
                if (min !== undefined) input.min = String(min);
                if (max !== undefined) input.max = String(max);
            };
            setBounds('display-width', limits.widthMin, limits.widthMax);
            setBounds('display-height', limits.heightMin, limits.heightMax);
            setBounds('display-buffer-lines', limits.bufferLinesMin, limits.bufferLinesMax);
            setBounds('display-brightness', limits.brightnessMin, limits.brightnessMax);
            setBounds('display-spi-frequency', limits.spiFrequencyMin, limits.spiFrequencyMax);
            var fillSelect = function(id, options, current, labels) {
                var select = document.getElementById(id);
                if (!select || !Array.isArray(options) || options.length === 0) return;
                current = current ?? select.value;
                select.innerHTML = options.map(function(option) {
                    var value = typeof option === 'object' ? (option.value || option.driver || option.backend || option.id || option.name) : option;
                    var label = typeof option === 'object' ? (option.label || option.displayName || value) : ((labels && labels[value]) || value);
                    return '<option value="' + escapeHtml(String(value)) + '">' + escapeHtml(String(label)) + '</option>';
                }).join('');
                if (current && Array.from(select.options).some(function(option) { return option.value === String(current); })) select.value = String(current);
            };
            var drivers = descriptor.driverOptions || [];
            var backends = Array.from(new Set(drivers.map(function(driver) { return driver.backend || 'spi_tft'; })));
            fillSelect('display-backend', backends, params?.backend, { spi_tft: 'SPI TFT', rgb: 'RGB565 (PSRAM)' });
            var backend = params?.backend || document.getElementById('display-backend')?.value || 'spi_tft';
            fillSelect('display-driver', drivers.filter(function(driver) { return !driver.backend || driver.backend === backend; }), params?.driver,
                { st7789: 'ST7789', ili9341: 'ILI9341', st7701: 'ST7701' });
            fillSelect('display-touch-backend', descriptor.touchOptions, params?.touchBackend, { none: '无触摸', gt911: 'GT911' });
            fillSelect('display-i2c-frequency', limits.i2cFrequencies, params?.i2cFrequency, { 100000: '100 kHz', 400000: '400 kHz' });
            var values = params ? {
                'display-backend': params.backend,
                'display-driver': params.driver,
                'display-touch-backend': params.touchBackend,
                'display-width': params.width,
                'display-height': params.height,
                'display-rotation': params.rotation,
                'display-color-depth': params.colorDepth,
                'display-buffer-lines': params.bufferLines,
                'display-brightness': params.brightness,
                'display-spi-frequency': params.spiFrequency,
                'display-i2c-frequency': params.i2cFrequency,
                'display-mosi': pins.mosi,
                'display-sclk': pins.sclk,
                'display-cs': pins.cs,
                'display-dc': pins.dc,
                'display-rst': pins.rst,
                'display-backlight': pins.backlight,
                'display-touch-sda': pins.touchSda,
                'display-touch-scl': pins.touchScl,
                'display-touch-int': pins.touchInt,
                'display-touch-rst': pins.touchRst,
                'display-backlight-channel': params.backlightChannel
            } : {};
            Object.keys(values).forEach(function(id) {
                var input = document.getElementById(id);
                if (input && values[id] !== undefined && values[id] !== null) input.value = String(values[id]);
            });
            var selectedDriver = drivers.find(function(driver) {
                return (driver.driver || driver.value) === document.getElementById('display-driver')?.value;
            });
            setBounds('display-width', selectedDriver?.minWidth ?? limits.widthMin, selectedDriver?.maxWidth ?? limits.widthMax);
            setBounds('display-height', selectedDriver?.minHeight ?? limits.heightMin, selectedDriver?.maxHeight ?? limits.heightMax);
            if (backend === 'rgb') {
                document.getElementById('display-width').value = '480';
                document.getElementById('display-height').value = '480';
            }
            var rgbFields = document.getElementById('display-rgb-fields');
            var rgbInputs = document.getElementById('display-rgb-inputs');
            if (rgbInputs && !rgbInputs.children.length) {
                var fields = [
                    ['rgbInitProfile', '初始化序列', 'st7701_type1'],
                    ['rgbFrequency', '像素时钟 Hz (1M-16M)', 12000000],
                    ['hsyncFrontPorch', '水平前沿', 10], ['hsyncPulseWidth', '水平脉宽', 8], ['hsyncBackPorch', '水平后沿', 50],
                    ['vsyncFrontPorch', '垂直前沿', 10], ['vsyncPulseWidth', '垂直脉宽', 8], ['vsyncBackPorch', '垂直后沿', 20],
                    ['hsyncIdleLow', 'HSYNC 空闲低', false], ['vsyncIdleLow', 'VSYNC 空闲低', false], ['pclkActiveNeg', 'PCLK 下降沿有效', false],
                    ['controlBackend', '辅助控制器', 'none'], ['controlAddress', '控制器地址 (十进制)', 0],
                    ['controlResetMask', '复位掩码', 0], ['controlBacklightMask', '背光掩码', 0], ['controlEnableMask', '使能掩码', 0],
                    ['controlBuzzerMask', '蜂鸣器掩码（仅 CH32V003）', 0],
                    ['touchBuzzerDurationMs', '触摸提示音时长', 30],
                    ['controlInactiveMask', '保持低电平掩码（与其他掩码不重叠）', 0],
                    ['controlTouchIntMask', 'GT911 INT 同步掩码（单一位）', 0],
                    ['controlSda', '控制 SDA', -1], ['controlScl', '控制 SCL', -1],
                    ['rgbDe', 'DE', -1], ['rgbVsync', 'VSYNC', -1], ['rgbHsync', 'HSYNC', -1], ['rgbPclk', 'PCLK', -1]
                ];
                for (var index = 0; index < 16; index++) fields.push(['rgbData' + index, 'D' + index, -1]);
                rgbInputs.innerHTML = fields.map(function(field) {
                    var name = field[0], label = field[1], value = field[2], id = 'display-' + name;
                    var input = name === 'controlBackend' ? '<select id="' + id + '"><option value="none">无</option><option value="tca9554">TCA9554</option><option value="ch32v003">CH32V003</option></select>' :
                        '<input id="' + id + '" type="' + (typeof value === 'boolean' ? 'checkbox' : (typeof value === 'number' ? 'number' : 'text')) +
                        '" value="' + value + '"' + (name === 'rgbInitProfile' ? ' readonly' : '') + '>';
                    if (name === 'touchBuzzerDurationMs') {
                        input = '<select id="' + id + '">' + [20, 30, 60, 100, 150].map(function(ms) {
                            return '<option value="' + ms + '"' + (ms === 30 ? ' selected' : '') + '>' + ms + ' ms</option>';
                        }).join('') + '</select><small class="fb-text-muted">板载蜂鸣器为固定音量；缩短提示音可减少打扰。仅用于触摸反馈。</small>';
                    }
                    return '<div class="fb-form-group"><label for="' + id + '">' + label + '</label>' + input + '</div>';
                }).join('');
            }
            if (rgbFields) {
                rgbFields.classList.toggle('is-hidden', backend !== 'rgb');
                rgbFields.querySelectorAll('input,select').forEach(function(input) {
                    input.disabled = backend !== 'rgb';
                    var key = input.id.slice(8);
                    var dataIndex = key.match(/^rgbData(\d+)$/);
                    var value = dataIndex ? pins.rgbDataPins?.[Number(dataIndex[1])] : (params?.[key] ?? pins[key]);
                    if (key === 'touchBuzzerDurationMs') {
                        value = params?.[key] ?? 30;
                        if (Number.isInteger(value) && value >= 20 && value <= 150 &&
                            !Array.from(input.options).some(function(option) { return option.value === String(value); })) {
                            input.add(new Option(value + ' ms', String(value)));
                        }
                    }
                    if (value !== undefined) {
                        if (input.type === 'checkbox') input.checked = value === true;
                        else input.value = String(value);
                    }
                });
            }
            if (params && params.backlightActiveHigh !== undefined) {
                var activeHigh = document.getElementById('display-backlight-active-high');
                if (activeHigh) activeHigh.checked = !!params.backlightActiveHigh;
            }
            var touchBackend = document.getElementById('display-touch-backend')?.value || 'none';
            var touchFields = document.getElementById('display-touch-fields');
            if (touchFields) {
                touchFields.classList.toggle('is-hidden', touchBackend === 'none');
                touchFields.querySelectorAll('input, select').forEach(function(input) { input.disabled = touchBackend === 'none'; });
            }
            var backlight = Number(document.getElementById('display-backlight')?.value ?? -1);
            var backlightFields = document.getElementById('display-backlight-fields');
            if (backlightFields) {
                var available = Number.isInteger(backlight) && backlight >= 0;
                backlightFields.classList.toggle('is-hidden', !available);
                backlightFields.querySelectorAll('input, select').forEach(function(input) { input.disabled = !available; });
            }
        },

        _updatePca9685Fields(saved) {
            this._setPeripheralReferenceOptions('pca9685-bus-id', 2, saved?.busPeripheralId);
            this._setPeripheralReferenceOptions('pca9685-oe-id', 12, saved?.oePeripheralId);
            const descriptor = this._getPeripheralTypeDescriptor(57);
            const aliases = {busPeripheralId: 'bus-id', oePeripheralId: 'oe-id'};
            for (const field of descriptor?.configFields || []) {
                const input = document.getElementById('pca9685-' + (aliases[field.key] || field.key));
                const initialize = input && input._h1ConfigSession !== this._periphFormSession;
                this._applyH1ConfigInput(input, field, saved ? saved[field.key] : field.default,
                    field.type !== 'reference' && (saved !== undefined || initialize));
                if (input) input._h1ConfigSession = this._periphFormSession;
            }
            this._setFeatureAvailabilityHint('pca9685-feature-status', this._getPeripheralTypeDescriptor(57), 'PCA9685 PWM');
            if (descriptor?.configSchemaVersion !== 1) {
                const hint = document.getElementById('pca9685-feature-status');
                if (hint) hint.textContent = '固件未提供 PCA9685 参数描述，请刷新并使用配套固件后保存';
            }
            this._updatePeripheralPinRoleHint(57);
        },

        _applyH1ConfigInput(input, field, value, replace) {
            if (!input) return;
            input.required = field.required === true;
            for (const attr of ['min', 'max', 'step', 'maxlength']) input.removeAttribute(attr);
            if (field.type === 'integer' || field.type === 'number') {
                if (field.min !== undefined) input.min = String(field.min);
                if (field.exclusiveMin !== undefined) input.min = String(field.exclusiveMin);
                if (field.max !== undefined) input.max = String(field.max);
                input.step = field.type === 'integer' ? '1' : 'any';
            }
            if (replace) {
                if (field.type === 'boolean') input.checked = value === true;
                else input.value = value === undefined ? '' : String(value);
            }
        },

        _updateIoExpanderFields(applyDefaults, saved) {
            const select = document.getElementById('io-expander-driver');
            if (!select) return;
            this._fillDriverSelect(select, 55, saved?.driverName || saved?.driver || select.value, false);
            const meta = this._getPeripheralDriverMeta(55, select.value);
            this._setPeripheralReferenceOptions('io-expander-bus-id', 2, saved?.busPeripheralId);
            this._setPeripheralReferenceOptions('io-expander-reset-id', 12, saved?.resetPeripheralId);
            document.getElementById('io-expander-reset-fields')?.classList.toggle('is-hidden', !meta?.requiresResetReference);
            document.getElementById('io-expander-group-hint')?.classList.toggle('is-hidden', !meta?.groupActivation);
            const address = document.getElementById('io-expander-address');
            if (address && saved) address.value = this._formatI2cAddress(saved.i2cAddress ?? saved.address);
            else if (address && meta && (applyDefaults || address._h1ConfigSession !== this._periphFormSession))
                address.value = this._formatI2cAddress(meta.defaultAddress);
            if (address && meta) address._h1ConfigSession = this._periphFormSession;
            const interval = document.getElementById('io-expander-interval');
            const intervalField = meta?.configFields?.find(field => field.key === 'sampleInterval');
            if (interval && intervalField) this._applyH1ConfigInput(interval, intervalField,
                saved?.sampleInterval === 0 ? intervalField.default : saved?.sampleInterval ?? intervalField.default,
                !!saved || applyDefaults || interval._h1ConfigSession !== this._periphFormSession);
            if (interval && intervalField) interval._h1ConfigSession = this._periphFormSession;
            this._setFeatureAvailabilityHint('io-expander-feature-status', this._getPeripheralTypeDescriptor(55), '通用 IO 扩展器');
            this._updatePeripheralPinRoleHint(55);
        },

        _updateGpioSourceFields(savedChannel) {
            const type = Number(document.getElementById('peripheral-type-input')?.value);
            const supported = type === 11 || type === 12;
            document.getElementById('gpio-source-fields')?.classList.toggle('is-hidden', !supported);
            const source = document.getElementById('gpio-io-source');
            if (!source) return;
            const option = Array.from(source.options).find((o) => o.value === 'expander');
            const descriptor = this._getPeripheralTypeDescriptor(55);
            if (option) option.disabled = !supported || !descriptor || descriptor.implemented !== true;
            if (!supported) source.value = 'gpio';
            const expanded = supported && source.value === 'expander';
            document.getElementById('gpio-expander-fields')?.classList.toggle('is-hidden', !expanded);
            if (supported) {
                (expanded || type === 12) ? this.hideElement('gpio-input-mode-group') : this.showElement('gpio-input-mode-group');
                (!expanded && type === 12) ? this.hideElement('gpio-invert-logic-group') : this.showElement('gpio-invert-logic-group');
            }
            const channels = document.getElementById('gpio-provider-channel');
            const selected = savedChannel === undefined ? (channels?.value || '0') : String(savedChannel);
            const parentId = document.getElementById('gpio-provider-id')?.value;
            const parent = (this._periphProviders || []).find((p) => p.id === parentId);
            const meta = this._getPeripheralDriverMeta(55, parent?.params?.driverName || parent?.params?.driver || 'PCF8574');
            const count = Number(meta?.channelCount || descriptor?.channelCount || meta?.channels?.length || 8);
            if (channels) {
                channels.innerHTML = Array.from({ length: Math.min(32, count) }, (_, i) => i)
                    .filter((i) => type !== 11 || !(meta?.outputOnlyChannels || []).includes(i))
                    .map((i) => '<option value="' + i + '">P' + i + '</option>').join('');
                if (!Array.from(channels.options).some((option) => option.value === selected)) {
                    const unavailable = document.createElement('option');
                    unavailable.value = selected;
                    unavailable.textContent = 'P' + selected + '（通道不可用）';
                    unavailable.disabled = true;
                    channels.appendChild(unavailable);
                }
                channels.value = selected;
            }
            const status = document.getElementById('gpio-source-status');
            if (status) status.textContent = expanded
                ? (descriptor?.featureActive === false ? 'IO 扩展功能未启用；配置保留，启用并重启后执行。' : (!parentId ? '先配置父扩展器，再绑定逻辑通道。' : (meta?.groupActivation ? 'MCP 通道先保持禁用保存，从父设备整组启用。7、15 只允许输出；输入没有上拉，输出初始逻辑值受反相设置影响。' : (type === 12 ? '初始状态按逻辑值处理；勾选反相后输出电平相反。' : '使用父设备采样；输入为释放高电平，不使用 MCU 内部上下拉。'))))
                : '';
            this._updatePeripheralPinRoleHint(type);
        },

        _refreshAdsScanFields(meta) {
            if (String(meta?.name).toUpperCase() !== 'ADS1115') return;
            const mask = Number(document.getElementById('generic-driver-scanMask')?.value || 0);
            const count = Array.from({length:8}, (_, bit) => !!(mask & (1 << bit))).filter(Boolean).length;
            document.querySelectorAll('[data-ads-mux]').forEach(node => { node.disabled = count >= 4 && !node.checked; });
            const single = document.getElementById('generic-driver-mux');
            if (single) single.disabled = !!mask;
            const channel = document.getElementById('generic-sensor-channel');
            const available = (meta.channels || []).filter(item =>
                (!item.singleMuxOnly || !mask) && (item.scanMux === undefined || !!(mask & (1 << Number(item.scanMux)))));
            if (channel) {
                const selected = channel.value;
                channel.innerHTML = '';
                available.forEach(item => {
                    const option = document.createElement('option');
                    option.value = item.name;
                    option.textContent = item.name + (item.unit ? ' (' + item.unit + ')' : '');
                    channel.appendChild(option);
                });
                if (available.some(item => item.name === selected)) channel.value = selected;
            }
            const rate = Number(document.getElementById('generic-driver-dataRate')?.value || 128);
            const interval = document.getElementById('generic-sensor-interval');
            if (interval) interval.min = String(Math.max(100, (count || 1) * (2 * Math.ceil(1000 / rate) + 50)));
        },

        _updateGenericSensorDriverFields(applyDefaults, savedParams) {
            var select = document.getElementById('generic-sensor-driver');
            var fields = document.getElementById('generic-sensor-i2c-fields');
            if (!select || !fields) return;
            if (applyDefaults) this._pendingSensorDriverParams = null;
            if (!savedParams && !applyDefaults && this._pendingSensorDriverParams) savedParams = this._pendingSensorDriverParams;
            var descriptor = this._getPeripheralTypeDescriptor(38);
            var previous = savedParams?.driverName || savedParams?.driver || select.value;
            this._fillDriverSelect(select, 38, previous, true);
            var driver = select.value.toLowerCase();
            var meta = this._getPeripheralDriverMeta(38, driver);
            if (savedParams) this._pendingSensorDriverParams = meta ? null : Object.assign({}, savedParams);
            fields.classList.toggle('is-hidden', !driver);
            ['generic-sensor-bus-id', 'generic-sensor-address', 'generic-sensor-channel'].forEach(function(id) {
                var input = document.getElementById(id);
                if (input) input.disabled = !driver;
            });
            this._setPeripheralReferenceOptions('generic-sensor-bus-id', 2, savedParams?.busPeripheralId);
            const address = document.getElementById('generic-sensor-address');
            if (address && savedParams && (savedParams.i2cAddress ?? savedParams.address) !== undefined) {
                address.value = this._formatI2cAddress(savedParams.i2cAddress ?? savedParams.address);
            } else if (address && applyDefaults && meta) address.value = this._formatI2cAddress(meta.defaultAddress);
            const channel = document.getElementById('generic-sensor-channel');
            if (channel) {
                // Channels are selected per read/action; the peripheral stores
                // only the MPU operating mode, never a default measurement.
                const selected = savedParams
                    ? (meta?.channels || []).find((item) => !item.mode || Number(item.mode) === Number(savedParams.mode || 0) + 1)?.name || ''
                    : (!applyDefaults ? channel.value : '');
                channel.innerHTML = (meta?.channels || []).map((item) => '<option value="' + escapeHtml(item.name) + '">' + escapeHtml((item.label || item.name) + (item.unit ? ' (' + item.unit + ')' : '')) + '</option>').join('');
                if (selected && !Array.from(channel.options).some((o) => o.value === selected)) {
                    const unavailable = document.createElement('option');
                    unavailable.value = selected;
                    unavailable.textContent = selected + '（通道不可用）';
                    unavailable.disabled = true;
                    channel.appendChild(unavailable);
                }
                if (selected) channel.value = selected;
            }
            this._renderDriverParams(meta, savedParams, applyDefaults);
            const interval = document.getElementById('generic-sensor-interval');
            if (interval) {
                interval.min = String(Math.max(100, Number(meta?.minIntervalMs) || 100));
                if (savedParams?.sampleInterval !== undefined) interval.value = savedParams.sampleInterval;
                else if (applyDefaults && Number(interval.value) < Number(interval.min)) interval.value = interval.min;
            }
            this._refreshAdsScanFields(meta);
            this._setFeatureAvailabilityHint('generic-sensor-feature-status', descriptor, driver ? 'I2C 传感器采集' : '');
            this._updatePeripheralPinRoleHint(38);
        },

        _updateSdcardPinHints() {
            var select = document.getElementById('sdcard-interface');
            if (!select) return;
            var descriptor = this._getPeripheralTypeDescriptor(37);
            var interfaces = this._descriptorStringList(descriptor, [
                'supportedInterfaces', 'interfaces', 'capabilities.interfaces'
            ]);
            var sdmmc = Array.from(select.options).find(function(option) { return option.value === '0'; });
            var sdmmcAvailable = !!descriptor && descriptor.implemented === true && interfaces.indexOf('sdmmc') >= 0;
            if (sdmmc) sdmmc.disabled = !sdmmcAvailable;
            if (select.value === '0' && !sdmmcAvailable) select.value = '1';
            var isSpi = select.value === '1';
            var hintSpi = document.getElementById('sdcard-pin-hint-spi');
            var hintSdmmc = document.getElementById('sdcard-pin-hint-sdmmc');
            if (hintSpi) hintSpi.classList.toggle('is-hidden', !isSpi);
            if (hintSdmmc) hintSdmmc.classList.toggle('is-hidden', isSpi);
            this._updatePeripheralPinRoleHint(37);
        },

        _resetPeripheralPreflight() {
            this._periphPreflightFingerprint = '';
            this._periphPreflightAcknowledgements = 0;
            var result = document.getElementById('peripheral-preflight-result');
            var saveBtn = document.getElementById('save-peripheral-btn');
            if (result) {
                result.textContent = '';
                result.className = 'u-hint-bar u-hint-bar-info is-hidden';
            }
            if (saveBtn && !this._periphSaveBusy) {
                saveBtn.textContent = t('peri-save', '保存');
            }
        },

        _getPinRecommendation(type) {
            var hw = this._periphHardware;
            if (!hw) return '';
            var pick = function(list, count) {
                return Array.isArray(list) ? list.slice(0, count || 8).join(',') : '';
            };
            var input = pick(hw.inputPins);
            var output = pick(hw.outputPins);
            var adc = pick(hw.adcPins);
            var dac = pick(hw.dacPins);
            var touch = pick(hw.touchPins);
            var chip = hw.chip || 'ESP32';

            if (type === 26 || type === 15) return adc ? chip + ' ADC 可用: ' + adc : chip + ' 不支持该 ADC 引脚配置';
            if (type === 27) return dac ? chip + ' DAC 可用: ' + dac : chip + ' 不支持 DAC';
            if (type === 21) return touch ? chip + ' Touch 可用: ' + touch : chip + ' 不支持 Touch';
            if ([11, 13, 14, 18, 19, 20, 44, 49, 53].indexOf(type) >= 0) {
                return input ? chip + ' 输入候选: ' + input : '';
            }
            if ([12, 16, 17, 41, 42, 45, 47, 48].indexOf(type) >= 0) {
                return output ? chip + ' 输出候选: ' + output : '';
            }
            if ([1, 2, 3, 36, 37, 43, 50, 52].indexOf(type) >= 0) {
                return output ? chip + ' 可用 GPIO 候选: ' + output : '';
            }
            return '';
        },

        editPeripheral(id) {
            if (!this.guardDeveloperModeAction()) return;
            this.openPeripheralModal(true, id);
        },

        async deletePeripheral(id) {
            if (!this.guardDeveloperModeAction()) return;
            if (!await FBConfirm({ message: t('peri-delete-confirm', '确定要删除外设') + ' ' + id + ' 吗？', danger: true })) return;
            apiDelete('/api/peripherals/', { id: id })
                .then(res => {
                    if (res && res.success) {
                        Notification.success(t('peri-delete-success', '外设已删除'), '外设配置');
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/peripherals');
                        }
                        this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '', { noCache: true });
                    } else {
                        Notification.error(res?.error || t('peri-delete-fail', '删除失败，请检查连接'), '外设配置');
                    }
                })
                .catch(err => {
                    console.error('Delete peripheral failed:', err);
                    Notification.error(t('peri-delete-fail', '删除失败，请检查连接'), '外设配置');
                });
        },

        _selectMcpGroupChannels(id, getter) {
            if (typeof this._selectMcpGroupChannelsLoaded === 'function')
                return this._selectMcpGroupChannelsLoaded(id, getter);
            const page = this.currentPage;
            return new Promise((resolve, reject) => {
                let pending = true;
                const timer = setTimeout(() => {
                    pending = false;
                    reject(new Error('整组确认模块加载超时，未启用设备'));
                }, 15000);
                const ready = () => {
                    if (!pending) return;
                    pending = false;
                    clearTimeout(timer);
                    if (this.currentPage !== page || typeof this._selectMcpGroupChannelsLoaded !== 'function') {
                        reject(new Error('页面已切换或模块不可用，未启用设备')); return;
                    }
                    resolve(this._selectMcpGroupChannelsLoaded(id, getter));
                };
                if (typeof ModuleLoader !== 'undefined' && typeof ModuleLoader.loadModule === 'function') {
                    ModuleLoader.loadModule('peripheral-io', ready);
                } else {
                    clearTimeout(timer); pending = false;
                    reject(new Error('整组确认模块不可用，未启用设备'));
                }
            });
        },

        async togglePeripheral(id) {
            if (!this.guardDeveloperModeAction() || this._periphToggleBusy) return;
            this._periphToggleBusy = true;
            try {
                var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
                var status = await getter('/api/peripherals/status', { id: id });
                if (!status || status.success === false || !status.data) {
                    throw new Error(status?.message || status?.error || '外设状态读取失败');
                }

                var isEnabled = status.data.enabled === true;
                var payload = { id: id };
                if (status.data.groupActivation === true) {
                    if (isEnabled) {
                        if (!await FBConfirm({ message: '整组禁用将停止所有通道并恢复初始电平。确认负载与接线后继续？', danger: true })) return;
                    } else {
                        const selected = await this._selectMcpGroupChannels(id, getter);
                        if (!selected) return;
                        payload.channelIds = JSON.stringify(selected.channelIds);
                        payload.groupToken = selected.groupToken;
                    }
                }
                if (!isEnabled) {
                    var post = (typeof apiPostSilent === 'function') ? apiPostSilent : apiPost;
                    var preflight = await post('/api/peripherals/preflight', {
                        operation: 'enable',
                        id: id
                    });
                    if (!preflight || preflight.success === false) {
                        throw new Error(preflight?.message || preflight?.error || '启用前资源预检失败');
                    }
                    var preflightData = preflight.data && typeof preflight.data === 'object'
                        ? preflight.data : preflight;
                    var issues = Array.isArray(preflightData.issues) ? preflightData.issues : [];
                    var hardErrors = issues.filter(function(issue) {
                        return String(issue?.severity || issue?.level || '').toLowerCase() === 'error';
                    });
                    if (preflightData.allowed === false || preflight.allowed === false || hardErrors.length) {
                        var errorText = hardErrors.map(function(issue) {
                            return issue.message || issue.code || String(issue);
                        }).join('\n') || preflight.message || '资源预检未通过，外设未启用';
                        Notification.error(errorText, '外设启用');
                        return;
                    }
                    var confirmationIssues = issues.filter(function(issue) {
                        var severity = String(issue?.severity || issue?.level || '').toLowerCase();
                        return issue?.requiresConfirmation === true || severity.indexOf('confirm') >= 0;
                    });
                    var requiresConfirmation = preflightData.requiresConfirmation === true ||
                        preflight.requiresConfirmation === true || confirmationIssues.length > 0;
                    var acknowledgements = Number(
                        preflightData.requiredAcknowledgements ?? preflight.requiredAcknowledgements ?? 0
                    ) || 0;
                    if (requiresConfirmation) {
                        var warningText = confirmationIssues.map(function(issue) {
                            return issue.message || issue.code || String(issue);
                        }).join('\n') || '该外设将使用有风险或共享的板卡资源。';
                        if (!await FBConfirm({ message: '预检提示：\n' + warningText + '\n\n确认接线与资源影响后继续启用？', danger: true })) return;
                        payload.acknowledgements = acknowledgements;
                    }
                }

                var endpoint = isEnabled ? '/api/peripherals/disable' : '/api/peripherals/enable';
                var result = await apiPost(endpoint, payload);
                if (!result || result.success === false) {
                    throw new Error(result?.message || result?.error || '状态切换失败');
                }
                Notification.success(
                    isEnabled ? t('peri-toggle-disabled', '外设已禁用') : t('peri-toggle-enabled', '外设已启用'),
                    '外设配置'
                );
                if (typeof window.apiInvalidateCache === 'function') {
                    window.apiInvalidateCache('/api/peripherals');
                }
                await this.loadPeripherals(
                    document.getElementById('peripheral-filter-type')?.value || '',
                    { noCache: true }
                );
            } catch (err) {
                console.error('Toggle peripheral failed:', err);
                Notification.error(
                    err?.data?.message || err?.data?.error || err?.message || t('peri-toggle-fail', '状态切换失败'),
                    '外设配置'
                );
            } finally {
                this._periphToggleBusy = false;
            }
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupPeripheralsEvents === 'function') {
        AppState.setupPeripheralsEvents();
    }
})();
