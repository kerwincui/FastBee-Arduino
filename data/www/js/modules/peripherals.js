/**
 * 外设接口管理模块
 * 包含外设列表、新增/编辑/删除外设、类型参数配置
 */
(function() {
    AppState.registerModule('peripherals', {

        // ============ 状态变量 ============
        _periphCurrentPage: 1,
        _periphPageSize: 10,
        _periphTotalCount: 0,

        // ============ 事件绑定 ============
        setupPeripheralsEvents() {
            // 新增外设按钮
            var addPeripheralBtn = document.getElementById('add-peripheral-btn');
            if (addPeripheralBtn) addPeripheralBtn.addEventListener('click', () => this.openPeripheralModal());
            // 关闭外设模态框
            var closePeripheralModal = document.getElementById('close-peripheral-modal');
            if (closePeripheralModal) closePeripheralModal.addEventListener('click', () => this.closePeripheralModal());
            // 取消外设按钮
            var cancelPeripheralBtn = document.getElementById('cancel-peripheral-btn');
            if (cancelPeripheralBtn) cancelPeripheralBtn.addEventListener('click', () => this.closePeripheralModal());
            // 保存外设按钮
            var savePeripheralBtn = document.getElementById('save-peripheral-btn');
            if (savePeripheralBtn) savePeripheralBtn.addEventListener('click', () => this.savePeripheralConfig());
            // 外设类型选择变化
            var peripheralTypeInput = document.getElementById('peripheral-type-input');
            if (peripheralTypeInput) peripheralTypeInput.addEventListener('change', (e) => this.onPeripheralTypeChange(e.target.value));
            // 外设过滤器
            var peripheralFilter = document.getElementById('peripheral-filter-type');
            if (peripheralFilter) peripheralFilter.addEventListener('change', (e) => this.loadPeripherals(e.target.value));
            // 兼容旧版：GPIO配置事件绑定
            var addGpioBtn = document.getElementById('add-gpio-btn');
            if (addGpioBtn) addGpioBtn.addEventListener('click', () => this.openPeripheralModal());
            var closeGpioModal = document.getElementById('close-gpio-modal');
            if (closeGpioModal) closeGpioModal.addEventListener('click', () => this.closePeripheralModal());
            var cancelGpioBtn = document.getElementById('cancel-gpio-btn');
            if (cancelGpioBtn) cancelGpioBtn.addEventListener('click', () => this.closePeripheralModal());
            var saveGpioBtn = document.getElementById('save-gpio-btn');
            if (saveGpioBtn) saveGpioBtn.addEventListener('click', () => this.savePeripheralConfig());
        },

        // ============ 外设列表 ============

        loadPeripherals(filterType = '') {
            const tbody = document.getElementById('peripheral-table-body');
            if (!tbody) return;
            tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #999;">' + i18n.t('peripheral-loading') + '</td></tr>';
            let url = '/api/peripherals?page=' + this._periphCurrentPage + '&pageSize=' + this._periphPageSize;
            if (filterType) url += '&category=' + filterType;
            apiGet(url)
                .then(res => {
                    if (!res || !res.success) {
                        tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #f56c6c;">' + i18n.t('peripheral-load-fail') + '</td></tr>';
                        this._renderPeriphPagination(0, 1, 10);
                        return;
                    }
                    const total = res.total || 0;
                    const page = res.page || 1;
                    const pageSize = res.pageSize || 10;
                    this._periphTotalCount = total;
                    const peripherals = res.data || [];
                    if (peripherals.length === 0) {
                        tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #999;">' + i18n.t('peripheral-empty') + '</td></tr>';
                        this._renderPeriphPagination(total, page, pageSize);
                        return;
                    }
                    let html = '';
                    peripherals.forEach(periph => {
                        const statusColors = { 0: '#999', 1: '#faad14', 2: '#52c41a', 3: '#1890ff', 4: '#ff4d4f' };
                        const statusNames = {
                            0: i18n.t('peripheral-status-disabled'), 1: i18n.t('peripheral-status-enabled'),
                            2: i18n.t('peripheral-status-initialized'), 3: i18n.t('peripheral-status-running'),
                            4: i18n.t('peripheral-status-error')
                        };
                        const statusColor = statusColors[periph.status] || '#999';
                        const statusName = statusNames[periph.status] || i18n.t('peripheral-status-unknown');
                        const pinsStr = periph.pins ? periph.pins.join(', ') : '--';
                        html += '<tr>' +
                            '<td>' + periph.id + '</td>' +
                            '<td>' + periph.name + '</td>' +
                            '<td>' + (periph.typeName || periph.type) + '</td>' +
                            '<td>' + pinsStr + '</td>' +
                            '<td style="color: ' + statusColor + ';">' + statusName + '</td>' +
                            '<td>' +
                                '<button class="btn btn-sm btn-edit" onclick="app.editPeripheral(\'' + periph.id + '\')">' + i18n.t('peripheral-edit') + '</button>' +
                                '<button class="btn btn-sm ' + (periph.enabled ? 'btn-disable' : 'btn-enable') + '" onclick="app.togglePeripheral(\'' + periph.id + '\')">' + (periph.enabled ? i18n.t('peripheral-disable') : i18n.t('peripheral-enable')) + '</button>' +
                                '<button class="btn btn-sm btn-delete" onclick="app.deletePeripheral(\'' + periph.id + '\')">' + i18n.t('peripheral-delete') + '</button>' +
                            '</td></tr>';
                    });
                    tbody.innerHTML = html;
                    this._renderPeriphPagination(total, page, pageSize);
                })
                .catch(err => {
                    console.error('Load peripherals failed:', err);
                    tbody.innerHTML = '<tr><td colspan="6" style="text-align: center; color: #f56c6c;">' + i18n.t('peripheral-load-fail') + '</td></tr>';
                    this._renderPeriphPagination(0, 1, 10);
                });
        },

        _renderPeriphPagination(total, page, pageSize) {
            const container = document.getElementById('periph-pagination');
            if (!container) return;
            const totalPages = Math.ceil(total / pageSize);
            if (totalPages <= 1) {
                container.innerHTML = '<span style="color:#999;font-size:12px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
                return;
            }
            let html = '<div class="pagination" style="display:flex;justify-content:center;align-items:center;gap:5px;flex-wrap:wrap;">';
            if (page > 1) {
                html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + (page-1) + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">&laquo;</button>';
            } else {
                html += '<button class="btn btn-sm" disabled>&laquo;</button>';
            }
            const maxVisiblePages = 5;
            let startPage = Math.max(1, page - Math.floor(maxVisiblePages / 2));
            let endPage = Math.min(totalPages, startPage + maxVisiblePages - 1);
            if (endPage - startPage + 1 < maxVisiblePages) startPage = Math.max(1, endPage - maxVisiblePages + 1);
            if (startPage > 1) {
                html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=1;app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">1</button>';
                if (startPage > 2) html += '<span style="padding:0 5px;">...</span>';
            }
            for (let i = startPage; i <= endPage; i++) {
                if (i === page) {
                    html += '<button class="btn btn-sm btn-primary" disabled>' + i + '</button>';
                } else {
                    html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + i + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">' + i + '</button>';
                }
            }
            if (endPage < totalPages) {
                if (endPage < totalPages - 1) html += '<span style="padding:0 5px;">...</span>';
                html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + totalPages + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">' + totalPages + '</button>';
            }
            if (page < totalPages) {
                html += '<button class="btn btn-sm" onclick="app._periphCurrentPage=' + (page+1) + ';app.loadPeripherals(document.getElementById(\'peripheral-filter-type\')?.value || \'\')">&raquo;</button>';
            } else {
                html += '<button class="btn btn-sm" disabled>&raquo;</button>';
            }
            html += ' <span style="color:#999;font-size:12px;margin-left:10px;">' + i18n.t('periph-exec-total') + ': ' + total + '</span>';
            html += '</div>';
            container.innerHTML = html;
        },

        // ============ 外设模态框 ============

        openPeripheralModal(isEdit = false, peripheralId = null) {
            const modal = document.getElementById('peripheral-modal');
            const title = document.getElementById('peripheral-modal-title');
            const form = document.getElementById('peripheral-form');
            if (!modal) {
                this.openGpioModal(isEdit, peripheralId ? { pin: peripheralId } : null);
                return;
            }
            form?.reset();
            const errorEl = document.getElementById('peripheral-error');
            if (errorEl) errorEl.style.display = 'none';
            document.querySelectorAll('.peripheral-params-group').forEach(el => el.style.display = 'none');
            if (isEdit && peripheralId) {
                title.textContent = i18n.t('peripheral-edit-modal-title');
                this.loadPeripheralForEdit(peripheralId);
            } else {
                title.textContent = i18n.t('peripheral-add-modal-title');
                document.getElementById('peripheral-original-id').value = '';
                document.getElementById('peripheral-id-input').disabled = false;
                this.onPeripheralTypeChange('11');
            }
            modal.style.display = 'flex';
        },

        closePeripheralModal() {
            const modal = document.getElementById('peripheral-modal');
            if (modal) modal.style.display = 'none';
            const oldModal = document.getElementById('gpio-modal');
            if (oldModal) oldModal.style.display = 'none';
        },

        onPeripheralTypeChange(typeValue) {
            const type = parseInt(typeValue);
            document.querySelectorAll('.peripheral-params-group').forEach(el => el.style.display = 'none');
            const pinHint = document.getElementById('uart-pin-direction-hint');
            if (pinHint) pinHint.style.display = 'none';
            if (type >= 11 && type <= 21) {
                const gpioParams = document.getElementById('gpio-params');
                if (gpioParams) {
                    gpioParams.style.display = 'block';
                    gpioParams.querySelectorAll('.pwm-only').forEach(el => {
                        el.style.display = (type === 17 || type === 16) ? 'block' : 'none';
                    });
                    gpioParams.querySelectorAll('.input-only').forEach(el => {
                        el.style.display = (type === 11 || type === 13 || type === 14) ? 'block' : 'none';
                    });
                }
            } else if (type === 1) {
                const uartParams = document.getElementById('uart-params');
                if (uartParams) uartParams.style.display = 'block';
                if (pinHint) pinHint.style.display = 'block';
            } else if (type === 2) {
                const i2cParams = document.getElementById('i2c-params');
                if (i2cParams) i2cParams.style.display = 'block';
            } else if (type === 3) {
                const spiParams = document.getElementById('spi-params');
                if (spiParams) spiParams.style.display = 'block';
            } else if (type === 26) {
                const adcParams = document.getElementById('adc-params');
                if (adcParams) adcParams.style.display = 'block';
            } else if (type === 27) {
                const dacParams = document.getElementById('dac-params');
                if (dacParams) dacParams.style.display = 'block';
            }
        },

        loadPeripheralForEdit(id) {
            apiGet('/api/peripherals/', { id: id })
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
                            if (data.params.defaultDuty !== undefined) { const el = document.getElementById('gpio-default-duty'); if (el) el.value = data.params.defaultDuty; }
                            if (data.params.debounceMs !== undefined) { const el = document.getElementById('gpio-debounce-ms'); if (el) el.value = data.params.debounceMs; }
                            if (data.params.defaultValue !== undefined) { const el = document.getElementById('dac-default-value'); if (el) el.value = data.params.defaultValue; }
                        }
                    } else {
                        Notification.error(i18n.t('peripheral-load-fail'), i18n.t('peripheral-title'));
                    }
                })
                .catch(err => {
                    console.error('Load peripheral for edit failed:', err);
                    Notification.error(i18n.t('peripheral-load-fail'), i18n.t('peripheral-title'));
                });
        },

        savePeripheralConfig() {
            const originalId = document.getElementById('peripheral-original-id').value;
            const id = document.getElementById('peripheral-id-input').value.trim();
            const name = document.getElementById('peripheral-name-input').value.trim();
            const type = document.getElementById('peripheral-type-input').value;
            const enabled = document.getElementById('peripheral-enabled-input').checked ? 1 : 0;
            const pinsStr = document.getElementById('peripheral-pins-input').value.trim();
            const errEl = document.getElementById('peripheral-error');
            if (!name || !type || !pinsStr) {
                errEl.textContent = i18n.t('peripheral-validate-required');
                errEl.style.display = 'block';
                return;
            }
            const isEdit = originalId !== '';
            const finalId = isEdit ? originalId : (id || undefined);
            const data = { id: finalId, name: name, type: type, enabled: enabled, pins: pinsStr };
            const typeNum = parseInt(type);
            if (typeNum >= 11 && typeNum <= 21) {
                data.initialState = document.getElementById('gpio-initial-state')?.value || '0';
                if (typeNum === 17 || typeNum === 16) {
                    data.pwmFrequency = document.getElementById('gpio-pwm-freq')?.value || '1000';
                    data.pwmResolution = document.getElementById('gpio-pwm-resolution')?.value || '8';
                    data.defaultDuty = document.getElementById('gpio-default-duty')?.value || '0';
                }
                if (typeNum === 12 && (document.getElementById('gpio-action-mode')?.value === '2')) {
                    data.pwmFrequency = document.getElementById('gpio-pwm-freq')?.value || '1000';
                    data.pwmResolution = document.getElementById('gpio-pwm-resolution')?.value || '8';
                }
                if (typeNum === 11 || typeNum === 13 || typeNum === 14) {
                    data.debounceMs = document.getElementById('gpio-debounce-ms')?.value || '50';
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
            } else if (typeNum === 26) {
                data.resolution = document.getElementById('adc-resolution')?.value || '12';
                data.attenuation = document.getElementById('adc-attenuation')?.value || '3';
            } else if (typeNum === 27) {
                data.defaultValue = document.getElementById('dac-default-value')?.value || '0';
            }
            const saveBtn = document.getElementById('save-peripheral-btn');
            const origText = saveBtn?.textContent;
            if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = i18n.t('peripheral-saving-text'); }
            const url = isEdit ? '/api/peripherals/update' : '/api/peripherals';
            apiPost(url, data)
                .then(res => {
                    if (res && res.success) {
                        this.closePeripheralModal();
                        this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '');
                        Notification.success(isEdit ? i18n.t('peripheral-update-ok') : i18n.t('peripheral-add-ok'), i18n.t('peripheral-title'));
                    } else {
                        errEl.textContent = res?.error || i18n.t('peripheral-save-fail');
                        errEl.style.display = 'block';
                    }
                })
                .catch(err => {
                    console.error('Save peripheral failed:', err);
                    errEl.textContent = i18n.t('peripheral-save-fail');
                    errEl.style.display = 'block';
                })
                .finally(() => {
                    if (saveBtn) { saveBtn.disabled = false; saveBtn.textContent = origText; }
                });
        },

        editPeripheral(id) {
            this.openPeripheralModal(true, id);
        },

        deletePeripheral(id) {
            if (!confirm(i18n.t('peripheral-confirm-delete') + id + i18n.t('peripheral-confirm-suffix'))) return;
            apiDelete('/api/peripherals/?id=' + encodeURIComponent(id))
                .then(res => {
                    if (res && res.success) {
                        Notification.success(i18n.t('peripheral-deleted'), i18n.t('peripheral-title'));
                        this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '');
                    } else {
                        Notification.error(res?.error || i18n.t('peripheral-delete-fail'), i18n.t('peripheral-title'));
                    }
                })
                .catch(err => {
                    console.error('Delete peripheral failed:', err);
                    Notification.error(i18n.t('peripheral-delete-fail'), i18n.t('peripheral-title'));
                });
        },

        togglePeripheral(id) {
            apiGet('/api/peripherals/status', { id: id })
                .then(res => {
                    if (res && res.success && res.data) {
                        const isEnabled = res.data.enabled;
                        const url = isEnabled ? '/api/peripherals/disable' : '/api/peripherals/enable';
                        apiPost(url, { id: id })
                            .then(res2 => {
                                if (res2 && res2.success) {
                                    Notification.success(isEnabled ? i18n.t('peripheral-disabled') : i18n.t('peripheral-enabled'), i18n.t('peripheral-title'));
                                    this.loadPeripherals(document.getElementById('peripheral-filter-type')?.value || '');
                                } else {
                                    Notification.error(res2?.error || i18n.t('peripheral-toggle-fail'), i18n.t('peripheral-title'));
                                }
                            })
                            .catch(err => {
                                console.error('Toggle peripheral failed:', err);
                                Notification.error(i18n.t('peripheral-toggle-fail'), i18n.t('peripheral-title'));
                            });
                    }
                })
                .catch(err => {
                    console.error('Get peripheral status failed:', err);
                    Notification.error(i18n.t('peripheral-toggle-fail'), i18n.t('peripheral-title'));
                });
        }
    });

    // 自动绑定事件
    if (typeof AppState.setupPeripheralsEvents === 'function') {
        AppState.setupPeripheralsEvents();
    }
})();
