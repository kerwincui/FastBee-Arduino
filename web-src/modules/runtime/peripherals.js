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

        // ============ 状态变量 ============
        _periphCurrentPage: 1,
        _periphPageSize: 10,
        _periphTotalCount: 0,
        _periphProfile: null,
        _periphHardware: null,
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
            // 注册模态窗事件绑定器（模态窗 DOM 加载后由 _loadModals 触发）
            if (typeof this._registerModalBinder === 'function') {
                this._registerModalBinder('peripherals', () => this._bindPeripheralModalEvents());
            }
            this._loadPeripheralHardwareCapabilities();
            this._periphEventsBound = true;
        },

        /**
         * 绑定模态窗内的事件（模态窗 DOM 已就绪后调用）
         * 由 _loadModals → _rebindAllModalEvents 触发
         */
        _bindPeripheralModalEvents() {
            if (this._periphModalBound) return;
            // 外设模态框
            var closePeripheralModal = document.getElementById('close-peripheral-modal');
            if (closePeripheralModal) closePeripheralModal.addEventListener('click', () => this.closePeripheralModal());
            var cancelPeripheralBtn = document.getElementById('cancel-peripheral-btn');
            if (cancelPeripheralBtn) cancelPeripheralBtn.addEventListener('click', () => this.closePeripheralModal());
            var savePeripheralBtn = document.getElementById('save-peripheral-btn');
            if (savePeripheralBtn) savePeripheralBtn.addEventListener('click', () => this.savePeripheralConfig());
            var peripheralTypeInput = document.getElementById('peripheral-type-input');
            if (peripheralTypeInput) peripheralTypeInput.addEventListener('change', (e) => this.onPeripheralTypeChange(e.target.value));
            var adcSignalMode = document.getElementById('adc-signal-mode');
            if (adcSignalMode) adcSignalMode.addEventListener('change', (e) => this._applyAnalogSignalDefaults(e.target.value));
            var pinsInput = document.getElementById('peripheral-pins-input');
            if (pinsInput) pinsInput.addEventListener('blur', (e) => this._validatePinsInline());
            // 兼容旧版 GPIO 模态框
            var closeGpioModal = document.getElementById('close-gpio-modal');
            if (closeGpioModal) closeGpioModal.addEventListener('click', () => this.closePeripheralModal());
            var cancelGpioBtn = document.getElementById('cancel-gpio-btn');
            if (cancelGpioBtn) cancelGpioBtn.addEventListener('click', () => this.closePeripheralModal());
            var saveGpioBtn = document.getElementById('save-gpio-btn');
            if (saveGpioBtn) saveGpioBtn.addEventListener('click', () => this.savePeripheralConfig());
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
            if (!this.guardDeveloperModeAction()) return;
            if (action === 'edit') this.editPeripheral(id);
            else if (action === 'toggle') this.togglePeripheral(id);
            else if (action === 'delete') this.deletePeripheral(id);
        },

        // 懒加载红外遥控管理模块并初始化嵌入面板
        _initIrManagePanel() {
            var self = this;
            if (typeof ModuleLoader === 'undefined' || typeof ModuleLoader.loadModule !== 'function') return;
            ModuleLoader.loadModule('ir-remote', function() {
                if (typeof self.initIrManage === 'function') self.initIrManage();
            });
        },

        // 懒加载 RF 遥控管理模块并初始化嵌入面板
        _initRfManagePanel() {
            var self = this;
            if (typeof ModuleLoader === 'undefined' || typeof ModuleLoader.loadModule !== 'function') return;
            ModuleLoader.loadModule('rf-remote', function() {
                if (typeof self.initRfManage === 'function') self.initRfManage();
            });
        },

        _renderPeripheralActionButton(action, id, label, className) {
            var locked = !this.isDeveloperModeEnabled();
            var attrs = locked ? ' disabled title="' + escapeHtml(this.getDeveloperModeDisabledText()) + '"' : '';
            var lockClass = locked ? ' dev-mode-locked' : '';
            return '<button class="fb-btn fb-btn-sm fb-btn-compact ' + className + lockClass + '" data-peripheral-action="' + action + '" data-id="' + escapeHtml(id) + '"' + attrs + '>' + label + '</button>';
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
            if (profile && profile.max !== undefined) {
                summary += ' (' + (profile.used || 0) + '/' + profile.max + ')';
            }
            return summary;
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
            getter('/api/peripherals', params)
                .then(res => {
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
                        const statusName = statusNames[periph.status] || '未知';
                        const statusBadgeClass = statusBadgeClasses[periph.status] || 'badge-info';
                        const safeId = escapeHtml(periph.id);
                        const safeName = escapeHtml(periph.name);
                        const safeType = escapeHtml(periph.typeName || periph.type || '--');
                        const typeTag = periph.implemented === false ? ' <span class="badge badge-info" style="font-size:10px">待驱动</span>' : '';
                        const pinsStr = escapeHtml(periph.pins ? periph.pins.join(', ') : '--');
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
                            '</div></td></tr>';
                    });
                    tbody.innerHTML = html;
                    this._renderPeriphPagination(total, page, pageSize);
                    this.applyDeveloperModeState(tbody);
                })
                .catch(err => {
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
            const filterValue = document.getElementById('peripheral-filter-type')?.value || '';
            this.renderPagination('periph-pagination', {
                total,
                page,
                pageSize,
                maxVisiblePages: 3,
                summaryText: this._formatPeripheralCapacitySummary(total),
                onPageChange: (nextPage) => {
                    this._periphCurrentPage = nextPage;
                    this.loadPeripherals(filterValue);
                }
            });
        },

        // ============ 外设模态框 ============

        openPeripheralModal(isEdit = false, peripheralId = null) {
            if (!this.guardDeveloperModeAction()) return;
            if (!isEdit && this._periphProfile && Number(this._periphProfile.remaining) <= 0) {
                Notification.warning(t('peri-capacity-full', '当前资源档位外设数量已达上限'), '外设配置');
                return;
            }
            const modal = document.getElementById('peripheral-modal');
            const title = document.getElementById('peripheral-modal-title');
            const form = document.getElementById('peripheral-form');
            if (!modal) {
                this.openGpioModal(isEdit, peripheralId ? { pin: peripheralId } : null);
                return;
            }
            form?.reset();
            this.clearInlineError('peripheral-error');
            document.querySelectorAll('.peripheral-params-group').forEach(el => this.hideElement(el));
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
            // 关闭时销毁红外管理面板（停止学习轮询）
            if (typeof this.teardownIrManage === 'function') this.teardownIrManage();
            // 关闭时销毁 RF 管理面板
            if (typeof this.teardownRfManage === 'function') this.teardownRfManage();
            this.hideModal('peripheral-modal');
            this.hideModal('gpio-modal');
        },

        onPeripheralTypeChange(typeValue) {
            const type = parseInt(typeValue);
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
            } else if (type === 1) {
                const uartParams = document.getElementById('uart-params');
                this.showElement(uartParams);
                this.showElement(pinHint);
            } else if (type === 2) {
                const i2cParams = document.getElementById('i2c-params');
                this.showElement(i2cParams);
            } else if (type === 3) {
                const spiParams = document.getElementById('spi-params');
                this.showElement(spiParams);
            } else if (type === 26 || type === 15) {
                const adcParams = document.getElementById('adc-params');
                this.showElement(adcParams);
            } else if (type === 38) {
                const genericSensorParams = document.getElementById('generic-sensor-params');
                if (genericSensorParams) this.showElement(genericSensorParams);
            } else if (type === 27) {
                const dacParams = document.getElementById('dac-params');
                this.showElement(dacParams);
            } else if (type === 47) {
                const segParams = document.getElementById('segment-params');
                if (segParams) this.showElement(segParams);
            } else if (type === 42) {
                const stepperParams = document.getElementById('stepper-params');
                if (stepperParams) this.showElement(stepperParams);
            } else if (type === 45) {
                const neoParams = document.getElementById('neopixel-params');
                if (neoParams) this.showElement(neoParams);
            } else if (type === 48) {
                const rfParams = document.getElementById('rf-params');
                if (rfParams) this.showElement(rfParams);
                // RF 遥控管理区域
                const rfSection = document.getElementById('rf-manage-section');
                if (rfSection) {
                    this.showElement(rfSection);
                    const originalId = document.getElementById('peripheral-original-id')?.value || '';
                    const hintNew = document.getElementById('rf-manage-hint-new');
                    const content = document.getElementById('rf-manage-content');
                    if (originalId) {
                        if (hintNew) hintNew.style.display = 'none';
                        if (content) content.style.display = '';
                        this._initRfManagePanel();
                    } else {
                        if (hintNew) hintNew.style.display = '';
                        if (content) content.style.display = 'none';
                    }
                }
            } else if (type === 49) {
                const radarParams = document.getElementById('radar-params');
                if (radarParams) this.showElement(radarParams);
            } else if (type === 36) {
                const lcdParams = document.getElementById('lcd-params');
                if (lcdParams) this.showElement(lcdParams);
            } else if (type === 43) {
                // 编码器参数
                const encoderParams = document.getElementById('encoder-params');
                if (encoderParams) this.showElement(encoderParams);
            } else if (type === 53) {
                // 红外接收头：显示红外管理区域
                const irSection = document.getElementById('ir-manage-section');
                if (irSection) {
                    this.showElement(irSection);
                    const originalId = document.getElementById('peripheral-original-id')?.value || '';
                    const hintNew = document.getElementById('ir-manage-hint-new');
                    const content = document.getElementById('ir-manage-content');
                    if (originalId) {
                        // 编辑已有红外外设：显示管理内容并初始化
                        if (hintNew) hintNew.style.display = 'none';
                        if (content) content.style.display = '';
                        this._initIrManagePanel();
                    } else {
                        // 新增：仅显示提示
                        if (hintNew) hintNew.style.display = '';
                        if (content) content.style.display = 'none';
                    }
                }
            } else if (type === 37) {
                // SD卡参数
                const sdcardParams = document.getElementById('sdcard-params');
                if (sdcardParams) this.showElement(sdcardParams);
                // 根据接口模式切换引脚提示
                const sdcardInterface = document.getElementById('sdcard-interface');
                if (sdcardInterface) {
                    const hintSpi = document.getElementById('sdcard-pin-hint-spi');
                    const hintSdmmc = document.getElementById('sdcard-pin-hint-sdmmc');
                    const updateHint = () => {
                        const isSpi = sdcardInterface.value === '1';
                        if (hintSpi) isSpi ? this.showElement(hintSpi) : this.hideElement(hintSpi);
                        if (hintSdmmc) isSpi ? this.hideElement(hintSdmmc) : this.showElement(hintSdmmc);
                    };
                    sdcardInterface.addEventListener('change', updateHint);
                    updateHint();
                }
            }
            // DEVICE_EVENT (60) 和 Modbus (51) 无引脚配置，隐藏 pins 字段
            const pinsGroup = document.getElementById('peripheral-pins-group');
            if (pinsGroup) {
                (type === 60 || type === 51) ? this.hideElement(pinsGroup) : this.showElement(pinsGroup);
            }
            
            // 设备事件提示信息显示/隐藏
            const deviceEventHint = document.getElementById('device-event-hint');
            if (deviceEventHint) {
                (type === 60) ? this.showElement(deviceEventHint) : this.hideElement(deviceEventHint);
            }
            
            // 通用传感器提示信息显示/隐藏
            const genericSensorHint = document.getElementById('generic-sensor-hint');
            if (genericSensorHint) {
                (type === 38) ? this.showElement(genericSensorHint) : this.hideElement(genericSensorHint);
            }

            // DS1302 实时时钟提示信息显示/隐藏
            const ds1302Hint = document.getElementById('ds1302-hint');
            if (ds1302Hint) {
                (type === 50) ? this.showElement(ds1302Hint) : this.hideElement(ds1302Hint);
            }

            // LCD1602 字符液晶提示信息显示/隐藏
            const lcd1602Hint = document.getElementById('lcd1602-hint');
            if (lcd1602Hint) {
                (type === 52) ? this.showElement(lcd1602Hint) : this.hideElement(lcd1602Hint);
            }
        },

        loadPeripheralForEdit(id) {
            var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/peripherals/', { id: id })
                .then(res => {
                    if (res && res.success && res.data) {
                        const data = res.data;
                        const safeValue = (val, def = '') => (val !== undefined && val !== null) ? val : def;
                        document.getElementById('peripheral-original-id').value = safeValue(data.id);
                        document.getElementById('peripheral-id-input').value = safeValue(data.id);
                        document.getElementById('peripheral-id-input').disabled = true;
                        document.getElementById('peripheral-name-input').value = safeValue(data.name);
                        document.getElementById('peripheral-type-input').value = safeValue(data.type, '11');
                        document.getElementById('peripheral-enabled-input').checked = data.enabled ? true : false;
                        if (data.pins && Array.isArray(data.pins)) {
                            const validPins = data.pins.filter(p => p !== 255 && p !== undefined && p !== null);
                            document.getElementById('peripheral-pins-input').value = validPins.join(',');
                        } else {
                            document.getElementById('peripheral-pins-input').value = '';
                        }
                        this.onPeripheralTypeChange(data.type);
                        if (data.params) {
                            if (data.params.initialState !== undefined) document.getElementById('gpio-initial-state').value = data.params.initialState;
                            if (data.params.pwmFrequency !== undefined) document.getElementById('gpio-pwm-freq').value = data.params.pwmFrequency;
                            if (data.params.pwmResolution !== undefined) document.getElementById('gpio-pwm-resolution').value = data.params.pwmResolution;
                            if (data.params.baudRate !== undefined) document.getElementById('uart-baudrate').value = data.params.baudRate;
                            if (data.params.dataBits !== undefined) document.getElementById('uart-databits').value = data.params.dataBits;
                            if (data.params.stopBits !== undefined) document.getElementById('uart-stopbits').value = data.params.stopBits;
                            if (data.params.parity !== undefined) document.getElementById('uart-parity').value = data.params.parity;
                            if (data.params.frequency !== undefined) {
                                var i2cFreq = document.getElementById('i2c-frequency');
                                if (i2cFreq) i2cFreq.value = data.params.frequency;
                                var spiFreq = document.getElementById('spi-frequency');
                                if (spiFreq) spiFreq.value = data.params.frequency;
                            }
                            if (data.params.address !== undefined) document.getElementById('i2c-address').value = data.params.address;
                            if (data.params.mode !== undefined) document.getElementById('spi-mode').value = data.params.mode;
                            if (data.params.resolution !== undefined) document.getElementById('adc-resolution').value = data.params.resolution;
                            if (data.params.attenuation !== undefined) document.getElementById('adc-attenuation').value = data.params.attenuation;
                            { const el = document.getElementById('adc-sample-rate'); if (el && data.params.sampleRate !== undefined) el.value = String(data.params.sampleRate); }
                            // ADC 工业滤波参数（旧配置缺字段时保持默认：单次采样）
                            { const el = document.getElementById('adc-sample-count'); if (el && data.params.sampleCount !== undefined) el.value = String(data.params.sampleCount); }
                            { const el = document.getElementById('adc-filter-mode'); if (el && data.params.filterMode !== undefined) el.value = String(data.params.filterMode); }
                            // AI 工程量转换参数。旧配置缺字段时保留 form.reset() 的安全默认值。
                            [
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
                            // 通用传感器参数
                            { const el = document.getElementById('generic-sensor-type'); if (el && data.params.sensorType !== undefined) el.value = String(data.params.sensorType); }
                            { const el = document.getElementById('generic-sensor-interval'); if (el && data.params.sampleInterval !== undefined) el.value = String(data.params.sampleInterval); }
                            // AO 模拟量输出模式回显
                            { const el = document.getElementById('gpio-ao-mode'); if (el) el.checked = !!data.params.aoMode; }
                            { const el = document.getElementById('gpio-ao-min-pct'); if (el && data.params.aoMinPct !== undefined) el.value = data.params.aoMinPct; }
                            { const el = document.getElementById('gpio-ao-max-pct'); if (el && data.params.aoMaxPct !== undefined) el.value = data.params.aoMaxPct; }
                            if (data.params.defaultDuty !== undefined) { const el = document.getElementById('gpio-default-duty'); if (el) el.value = data.params.defaultDuty; }
                            if (data.params.debounceMs !== undefined) { const el = document.getElementById('gpio-debounce-ms'); if (el) el.value = data.params.debounceMs; }
                            if (data.params.inputMode !== undefined) { const el = document.getElementById('gpio-input-mode'); if (el) el.value = String(data.params.inputMode); }
                            { const el = document.getElementById('gpio-invert-logic'); if (el && data.params.invertLogic !== undefined) el.checked = !!data.params.invertLogic; }
                            if (data.params.reportMode !== undefined) { const el = document.getElementById('gpio-report-mode'); if (el) el.value = String(data.params.reportMode); }
                            if (data.params.defaultValue !== undefined) { const el = document.getElementById('dac-default-value'); if (el) el.value = data.params.defaultValue; }
                            if (data.params.brightness !== undefined) { const el = document.getElementById('segment-brightness'); if (el) el.value = data.params.brightness; }
                            if (data.params.stepsPerRevolution !== undefined) { const el = document.getElementById('stepper-steps-per-rev'); if (el) el.value = data.params.stepsPerRevolution; }
                            if (data.params.speed !== undefined) { const el = document.getElementById('stepper-speed'); if (el) el.value = data.params.speed; }
                            if (data.params.count !== undefined) { const el = document.getElementById('neopixel-count'); if (el) el.value = data.params.count; }
                            if (data.params.brightness !== undefined) { const el = document.getElementById('neopixel-brightness'); if (el) el.value = data.params.brightness; }
                            if (data.params.mode !== undefined) { const el = document.getElementById('rf-mode'); if (el) el.value = data.params.mode; }
                            if (data.params.bitLength !== undefined) { const el = document.getElementById('rf-bit-length'); if (el) el.value = data.params.bitLength; }
                            if (data.params.pulseWidth !== undefined) { const el = document.getElementById('rf-pulse-width'); if (el) el.value = data.params.pulseWidth; }
                            if (data.params.repeat !== undefined) { const el = document.getElementById('rf-repeat'); if (el) el.value = data.params.repeat; }
                            if (data.params.activeHigh !== undefined) {
                                const rfActive = document.getElementById('rf-active-high');
                                const radarActive = document.getElementById('radar-active-high');
                                if (rfActive) rfActive.checked = !!data.params.activeHigh;
                                if (radarActive) radarActive.checked = !!data.params.activeHigh;
                            }
                            if (data.params.debounceMs !== undefined) { const el = document.getElementById('radar-debounce-ms'); if (el) el.value = data.params.debounceMs; }
                            if (data.params.holdMs !== undefined) { const el = document.getElementById('radar-hold-ms'); if (el) el.value = data.params.holdMs; }
                            // OLED/LCD 参数回填
                            if (data.params.width !== undefined) { const el = document.getElementById('lcd-width'); if (el) el.value = data.params.width; }
                            if (data.params.height !== undefined) { const el = document.getElementById('lcd-height'); if (el) el.value = data.params.height; }
                            if (data.params.interface !== undefined) { const el = document.getElementById('lcd-interface'); if (el) el.value = data.params.interface; }
                            if (data.params.contrast !== undefined) { const el = document.getElementById('lcd-contrast'); if (el) el.value = data.params.contrast; }
                            // 编码器参数回填
                            if (data.params.resolution !== undefined) { const el = document.getElementById('encoder-resolution'); if (el) el.value = data.params.resolution; }
                            if (data.params.useInterrupt !== undefined) { const el = document.getElementById('encoder-use-interrupt'); if (el) el.checked = !!data.params.useInterrupt; }
                            // SD卡参数回填
                            if (data.params.sdcard && data.params.sdcard.interface !== undefined) { const el = document.getElementById('sdcard-interface'); if (el) el.value = data.params.sdcard.interface; }
                            if (data.params.sdcard && data.params.sdcard.frequency !== undefined) { const el = document.getElementById('sdcard-frequency'); if (el) el.value = data.params.sdcard.frequency; }
                        }
                    } else {
                        Notification.error(t('peri-load-fail', '加载失败'), '外设配置');
                    }
                })
                .catch(err => {
                    console.error('Load peripheral for edit failed:', err);
                    Notification.error(t('peri-load-fail', '加载失败'), '外设配置');
                });
        },

        _loadPeripheralHardwareCapabilities() {
            apiGet('/api/peripherals/types').then((res) => {
                if (res && res.success && res.hardware) {
                    this._periphHardware = res.hardware;
                }
            }).catch((err) => console.warn('Load peripheral hardware capabilities failed:', err));
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

        _validatePinsInline() {
            var typeEl = document.getElementById('peripheral-type-input');
            var pinsEl = document.getElementById('peripheral-pins-input');
            var hintEl = document.getElementById('peripheral-pins-validation-hint');
            if (!typeEl || !pinsEl) return;
            var type = typeEl.value;
            var pins = pinsEl.value.trim();
            if (!type || !pins) {
                if (hintEl) hintEl.innerHTML = '';
                return;
            }
            var excludeId = document.getElementById('peripheral-original-id')?.value || '';
            var self = this;
            apiGet('/api/peripherals/validate-pins', { type: type, pins: pins, excludeId: excludeId })
                .then(function(res) {
                    if (!hintEl) return;
                    if (!res || !res.success) {
                        hintEl.innerHTML = '<span class="u-text-danger">' + t('peri-validate-fail', '校验请求失败') + '</span>';
                        return;
                    }
                    if (!res.pins || res.pins.length === 0) {
                        hintEl.innerHTML = '';
                        return;
                    }
                    var msgs = [];
                    res.pins.forEach(function(p) {
                        if (p.error) msgs.push('GPIO' + p.pin + ': ' + p.error);
                        else if (p.warning) msgs.push('GPIO' + p.pin + ': ' + p.warning);
                    });
                    if (msgs.length === 0) {
                        hintEl.innerHTML = '<span style="color:var(--success)">\u2713 ' + t('peri-pin-available', '引脚可用') + '</span>';
                    } else {
                        var cls = res.allValid ? 'u-text-warning' : 'u-text-danger';
                        hintEl.innerHTML = '<span class="' + cls + '">' + msgs.map(function(m) { return escapeHtml(m); }).join('<br>') + '</span>';
                    }
                })
                .catch(function() {
                    if (hintEl) hintEl.innerHTML = '';
                });
        },

        savePeripheralConfig() {
            if (!this.guardDeveloperModeAction()) return;
            const originalId = document.getElementById('peripheral-original-id').value;
            const id = document.getElementById('peripheral-id-input').value.trim();
            const name = document.getElementById('peripheral-name-input').value.trim();
            const type = document.getElementById('peripheral-type-input').value;
            const enabled = document.getElementById('peripheral-enabled-input').checked ? 1 : 0;
            const pinsStr = document.getElementById('peripheral-pins-input').value.trim();
            const errEl = document.getElementById('peripheral-error');
            if (!name || !type || (!pinsStr && parseInt(type) !== 60 && parseInt(type) !== 51 && parseInt(type) !== 38)) {
                this.showInlineError(errEl, t('peri-validate-empty', '请填写外设ID和名称'));
                return;
            }
            this.clearInlineError(errEl);
            const isEdit = originalId !== '';
            const finalId = isEdit ? originalId : (id || undefined);
            const data = { id: finalId, name: name, type: type, enabled: enabled, pins: pinsStr };
            const typeNum = parseInt(type);
            if (typeNum >= 11 && typeNum <= 21 && typeNum !== 15) {
                data.initialState = document.getElementById('gpio-initial-state')?.value || '0';
                if (typeNum === 17 || typeNum === 16) {
                    data.pwmFrequency = document.getElementById('gpio-pwm-freq')?.value || '1000';
                    data.pwmResolution = document.getElementById('gpio-pwm-resolution')?.value || '8';
                    data.defaultDuty = document.getElementById('gpio-default-duty')?.value || '0';
                    // AO 模拟量输出模式：勾选后 PWM 按 0-100% 百分比映射
                    if (document.getElementById('gpio-ao-mode')?.checked) {
                        data.aoMode = true;
                        data.aoMinPct = parseInt(document.getElementById('gpio-ao-min-pct')?.value || '0') || 0;
                        data.aoMaxPct = parseInt(document.getElementById('gpio-ao-max-pct')?.value || '100');
                        if (isNaN(data.aoMaxPct)) data.aoMaxPct = 100;
                    }
                }
                if (typeNum === 12 && (document.getElementById('gpio-action-mode')?.value === '2')) {
                    data.passive = true;
                    data.pwmFrequency = document.getElementById('gpio-pwm-freq')?.value || '2700';
                    data.pwmResolution = document.getElementById('gpio-pwm-resolution')?.value || '8';
                }
                if (typeNum === 11 || typeNum === 13 || typeNum === 14 || typeNum === 18 || typeNum === 19 || typeNum === 20) {
                    data.debounceMs = document.getElementById('gpio-debounce-ms')?.value || '50';
                    data.inputMode = document.getElementById('gpio-input-mode')?.value || '0';
                    data.invertLogic = !!document.getElementById('gpio-invert-logic')?.checked;
                    data.reportMode = document.getElementById('gpio-report-mode')?.value || '0';
                }
            } else if (typeNum === 1) {
                data.baudRate = document.getElementById('uart-baudrate')?.value || '115200';
                data.dataBits = document.getElementById('uart-databits')?.value || '8';
                data.stopBits = document.getElementById('uart-stopbits')?.value || '1';
                data.parity = document.getElementById('uart-parity')?.value || '0';
            } else if (typeNum === 2) {
                data.frequency = document.getElementById('i2c-frequency')?.value || '100000';
                data.address = document.getElementById('i2c-address')?.value || '0';
            } else if (typeNum === 3) {
                data.frequency = document.getElementById('spi-frequency')?.value || '1000000';
                data.mode = document.getElementById('spi-mode')?.value || '0';
            } else if (typeNum === 26 || typeNum === 15) {
                data.resolution = document.getElementById('adc-resolution')?.value || '12';
                data.attenuation = document.getElementById('adc-attenuation')?.value || '3';
                // 工业滤波和 AI 转换字段始终提交，允许编辑时明确恢复为默认值。
                var readAdcNumber = function (id, fallback) {
                    var value = parseFloat(document.getElementById(id)?.value ?? '');
                    return isFinite(value) ? value : fallback;
                };
                var readAdcInteger = function (id, fallback, min, max) {
                    var value = parseInt(document.getElementById(id)?.value ?? '', 10);
                    if (isNaN(value)) value = fallback;
                    return Math.max(min, Math.min(max, value));
                };
                data.sampleRate = readAdcInteger('adc-sample-rate', 0, 0, 65535);
                data.sampleCount = readAdcInteger('adc-sample-count', 1, 1, 16);
                data.filterMode = readAdcInteger('adc-filter-mode', 0, 0, 1);
                data.signalMode = readAdcInteger('adc-signal-mode', 0, 0, 4);
                data.conversionEnabled = !!document.getElementById('adc-conversion-enabled')?.checked;
                data.clampEnabled = !!document.getElementById('adc-clamp-enabled')?.checked;
                data.decimalPlaces = readAdcInteger('adc-decimal-places', 2, 0, 6);
                data.inputMin = readAdcNumber('adc-input-min', 0);
                data.inputMax = readAdcNumber('adc-input-max', 0);
                data.engineeringMin = readAdcNumber('adc-engineering-min', 0);
                data.engineeringMax = readAdcNumber('adc-engineering-max', 100);
                data.engineeringOffset = readAdcNumber('adc-engineering-offset', 0);
                data.vRef = readAdcNumber('adc-vref', 0);
                data.voltageRatio = readAdcNumber('adc-voltage-ratio', 0);
                data.shuntResistance = readAdcNumber('adc-shunt-resistance', 0);
                data.inputGain = readAdcNumber('adc-input-gain', 0);
                data.inputOffset = readAdcNumber('adc-input-offset', 0);
                data.rawMin = readAdcInteger('adc-raw-min', 0, 0, 65535);
                data.rawMax = readAdcInteger('adc-raw-max', 0, 0, 65535);
            } else if (typeNum === 38) {
                var genericType = parseInt(document.getElementById('generic-sensor-type')?.value || '0', 10);
                if (isNaN(genericType) || genericType < 0) genericType = 0;
                if (genericType > 255) genericType = 255;
                var genericInterval = parseInt(document.getElementById('generic-sensor-interval')?.value || '1000', 10);
                if (isNaN(genericInterval) || genericInterval < 100) genericInterval = 1000;
                if (genericInterval > 86400000) genericInterval = 86400000;
                data.sensorType = genericType;
                data.sampleInterval = genericInterval;
            } else if (typeNum === 27) {
                data.defaultValue = document.getElementById('dac-default-value')?.value || '0';
            } else if (typeNum === 47) {
                data.brightness = document.getElementById('segment-brightness')?.value || '3';
            } else if (typeNum === 42) {
                data.stepsPerRevolution = document.getElementById('stepper-steps-per-rev')?.value || '2048';
                data.speed = document.getElementById('stepper-speed')?.value || '8';
            } else if (typeNum === 45) {
                data.count = document.getElementById('neopixel-count')?.value || '1';
                data.brightness = document.getElementById('neopixel-brightness')?.value || '64';
            } else if (typeNum === 48) {
                data.mode = document.getElementById('rf-mode')?.value || '0';
                data.bitLength = document.getElementById('rf-bit-length')?.value || '24';
                data.pulseWidth = document.getElementById('rf-pulse-width')?.value || '350';
                data.repeat = document.getElementById('rf-repeat')?.value || '8';
                data.activeHigh = document.getElementById('rf-active-high')?.checked ? '1' : '0';
            } else if (typeNum === 49) {
                data.activeHigh = document.getElementById('radar-active-high')?.checked ? '1' : '0';
                data.debounceMs = document.getElementById('radar-debounce-ms')?.value || '50';
                data.holdMs = document.getElementById('radar-hold-ms')?.value || '2000';
            } else if (typeNum === 36) {
                data.width = document.getElementById('lcd-width')?.value || '128';
                data.height = document.getElementById('lcd-height')?.value || '64';
                data.interface = document.getElementById('lcd-interface')?.value || '2';
                data.contrast = document.getElementById('lcd-contrast')?.value || '255';
            } else if (typeNum === 43) {
                // 编码器参数
                data.resolution = document.getElementById('encoder-resolution')?.value || '1024';
                data.useInterrupt = document.getElementById('encoder-use-interrupt')?.checked ? '1' : '0';
            } else if (typeNum === 37) {
                // SD卡参数
                data.interface = document.getElementById('sdcard-interface')?.value || '1';
                data.frequency = document.getElementById('sdcard-frequency')?.value || '20000000';
            }
            const saveBtn = document.getElementById('save-peripheral-btn');
            const origText = saveBtn?.textContent;
            if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = t('peri-saving', '保存中...'); }
            const url = isEdit ? '/api/peripherals/update' : '/api/peripherals';
            apiPost(url, data)
                .then(res => {
                    if (res && res.success) {
                        var newPeriphId = res.data && res.data.id ? res.data.id : (isEdit ? originalId : '');
                        this.closePeripheralModal();
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/peripherals');
                        }
                        this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '', { noCache: true });
                        Notification.success(isEdit ? t('peri-save-success-update', '外设更新成功') : t('peri-save-success-add', '外设添加成功'), '外设配置');
                    } else {
                        this.showInlineError(errEl, res?.error || t('peri-save-fail', '保存失败，请检查连接'));
                    }
                })
                .catch(err => {
                    console.error('Save peripheral failed:', err);
                    this.showInlineError(errEl, err?.data?.error || err?.data?.message || t('peri-save-fail', '保存失败，请检查连接'));
                })
                .finally(() => {
                    if (saveBtn) { saveBtn.disabled = false; saveBtn.textContent = origText; }
                });
        },

        editPeripheral(id) {
            if (!this.guardDeveloperModeAction()) return;
            this.openPeripheralModal(true, id);
        },

        deletePeripheral(id) {
            if (!this.guardDeveloperModeAction()) return;
            if (!confirm(t('peri-delete-confirm', '确定要删除外设') + ' ' + id + ' 吗？')) return;
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

        togglePeripheral(id) {
            if (!this.guardDeveloperModeAction()) return;
            // 优化：直接发送 toggle 请求，由后端处理状态翻转
            // 如果后端支持 /api/peripherals/toggle 则使用单次请求
            // 否则 fallback 到先获取状态再切换的方式
            apiPost('/api/peripherals/toggle', { id: id })
                .then(res => {
                    if (res && res.success) {
                        var msg = res.enabled ? t('peri-toggle-enabled', '外设已启用') : t('peri-toggle-disabled', '外设已禁用');
                        Notification.success(msg, '外设配置');
                        if (typeof window.apiInvalidateCache === 'function') {
                            window.apiInvalidateCache('/api/peripherals');
                        }
                        this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '', { noCache: true });
                    } else if (res && res.status === 404) {
                        this._togglePeripheralLegacy(id);
                    } else {
                        Notification.error(res?.error || t('peri-toggle-fail', '状态切换失败'), '外设配置');
                    }
                })
                .catch(err => {
                    if (err && (err.status === 404 || err.statusCode === 404)) {
                        this._togglePeripheralLegacy(id);
                    } else {
                        console.error('Toggle peripheral failed:', err);
                        Notification.error(t('peri-toggle-fail', '状态切换失败'), '外设配置');
                    }
                });
        },

        _togglePeripheralLegacy(id) {
            var getter = (typeof apiGetFresh === 'function') ? apiGetFresh : apiGet;
            getter('/api/peripherals/status', { id: id })
                .then(res => {
                    if (res && res.success && res.data) {
                        const isEnabled = res.data.enabled;
                        const url = isEnabled ? '/api/peripherals/disable' : '/api/peripherals/enable';
                        apiPost(url, { id: id })
                            .then(res2 => {
                                if (res2 && res2.success) {
                                    Notification.success(isEnabled ? t('peri-toggle-disabled', '外设已禁用') : t('peri-toggle-enabled', '外设已启用'), '外设配置');
                                    if (typeof window.apiInvalidateCache === 'function') {
                                        window.apiInvalidateCache('/api/peripherals');
                                    }
                                    this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '', { noCache: true });
                                } else {
                                    Notification.error(res2?.error || t('peri-toggle-fail', '状态切换失败'), '外设配置');
                                }
                            })
                            .catch(err => {
                                console.error('Toggle peripheral failed:', err);
                                Notification.error(t('peri-toggle-fail', '状态切换失败'), '外设配置');
                            });
                    }
                })
                .catch(err => {
                    console.error('Get peripheral status failed:', err);
                    Notification.error(t('peri-toggle-fail', '状态切换失败'), '外设配置');
                });
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupPeripheralsEvents === 'function') {
        AppState.setupPeripheralsEvents();
    }
})();
