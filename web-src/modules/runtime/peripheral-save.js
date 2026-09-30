/**
 * On-demand peripheral save validation and resource preflight.
 */
(function() {
    AppState.registerModule('peripheral-save', {
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

        _peripheralConfigFingerprint(data) {
            var normalized = {};
            Object.keys(data || {}).sort().forEach(function(key) { normalized[key] = data[key]; });
            return JSON.stringify(normalized);
        },

        _isUnsupportedPeripheralPreflight(err) {
            var status = Number(err && (err.status ?? err.statusCode ?? err.response?.status));
            return status === 404 || status === 405 || status === 501;
        },

        _normalizePeripheralPreflight(response) {
            var data = response && response.data !== undefined ? response.data : response;
            data = data && typeof data === 'object' ? data : {};
            var normalizeIssues = function(items, fallbackSeverity) {
                if (!Array.isArray(items)) return [];
                return items.map(function(issue) {
                    if (issue && typeof issue === 'object') {
                        return Object.assign({ severity: fallbackSeverity }, issue);
                    }
                    return { severity: fallbackSeverity, message: String(issue) };
                });
            };
            var issues = normalizeIssues(
                Array.isArray(data.issues) ? data.issues : response?.issues,
                'info'
            );
            var explicitErrors = normalizeIssues(
                Array.isArray(data.errors) ? data.errors : response?.errors,
                'error'
            );
            var explicitWarnings = normalizeIssues(
                Array.isArray(data.warnings) ? data.warnings : response?.warnings,
                'warning'
            );
            issues = issues.concat(explicitErrors, explicitWarnings);
            var severity = function(issue) { return String(issue?.severity || issue?.level || '').toLowerCase(); };
            var errors = issues.filter(function(issue) { return severity(issue) === 'error'; });
            var confirmationIssues = issues.filter(function(issue) {
                var level = severity(issue);
                return level.indexOf('confirm') >= 0 || issue?.requiresConfirmation === true || level === 'warning';
            });
            var allowed = data.valid !== false && response?.valid !== false &&
                data.allowed !== false && response?.allowed !== false && errors.length === 0;
            var requiresConfirmation = data.requiresConfirmation === true || response?.requiresConfirmation === true || confirmationIssues.length > 0;
            return {
                allowed: allowed,
                requiresConfirmation: requiresConfirmation,
                issues: issues,
                claims: Array.isArray(data.claims) ? data.claims : [],
                resourceDiff: data.resourceDiff || data.diff || null,
                acknowledgements: Number(data.requiredAcknowledgements ?? data.acknowledgements ?? response?.requiredAcknowledgements ?? 0) || 0
            };
        },

        _renderPeripheralPreflight(result, mode) {
            var element = document.getElementById('peripheral-preflight-result');
            if (!element) return;
            element.textContent = '';
            element.className = 'u-hint-bar ' + (mode === 'error' || mode === 'warning'
                ? 'u-hint-bar-warning' : (mode === 'success' ? 'u-hint-bar-success' : 'u-hint-bar-info'));
            var title = document.createElement('strong');
            title.textContent = mode === 'error' ? '保存前预检未通过'
                : (mode === 'legacy' ? '兼容模式' : (result.requiresConfirmation ? '预检需要确认' : '保存前预检通过'));
            element.appendChild(title);
            var details = [];
            (result.issues || []).forEach(function(issue) {
                details.push(issue.message || issue.code || String(issue));
            });
            if (result.resourceDiff) {
                var deltaCount = function(value) {
                    if (Array.isArray(value)) return value.length;
                    if (value && typeof value === 'object') return Object.keys(value).length;
                    return value;
                };
                var added = deltaCount(result.resourceDiff.added ?? result.resourceDiff.add ?? result.resourceDiff.addedCount);
                var removed = deltaCount(result.resourceDiff.removed ?? result.resourceDiff.remove ?? result.resourceDiff.removedCount);
                if (added !== undefined || removed !== undefined) details.push('资源变化: +' + (added || 0) + ' / -' + (removed || 0));
            } else if (result.claims && result.claims.length) {
                details.push('本次将占用 ' + result.claims.length + ' 项资源');
            }
            if (result.allowed && result.requiresConfirmation) {
                details.push('确认提示信息无误后，请再次点击“确认并保存”');
            }
            if (details.length) {
                var list = document.createElement('ul');
                details.forEach(function(detail) {
                    var item = document.createElement('li');
                    item.textContent = detail;
                    list.appendChild(item);
                });
                element.appendChild(list);
            }
        },

        async _runPeripheralPreflight(data) {
            const session = this._periphFormSession;
            var fingerprint = this._peripheralConfigFingerprint(data);
            if (this._periphPreflightFingerprint === fingerprint) {
                data.confirmWarnings = 1;
                if (this._periphPreflightAcknowledgements) data.acknowledgements = this._periphPreflightAcknowledgements;
                return true;
            }
            try {
                var post = (typeof apiPostSilent === 'function') ? apiPostSilent : apiPost;
                var response = await post('/api/peripherals/preflight', Object.assign({
                    operation: document.getElementById('peripheral-original-id')?.value ? 'update' : 'add'
                }, data));
                if (session !== this._periphFormSession) return false;
                if (!response || response.success === false) throw new Error(response?.message || response?.error || '预检失败');
                var result = this._normalizePeripheralPreflight(response);
                this._renderPeripheralPreflight(
                    result,
                    result.allowed ? (result.requiresConfirmation ? 'warning' : 'success') : 'error'
                );
                if (!result.allowed) {
                    return false;
                }
                if (result.requiresConfirmation) {
                    this._periphPreflightFingerprint = fingerprint;
                    this._periphPreflightAcknowledgements = result.acknowledgements;
                    return false;
                }
                this._periphPreflightFingerprint = '';
                this._periphPreflightAcknowledgements = 0;
                return true;
            } catch (err) {
                if (session !== this._periphFormSession) return false;
                if (!this._isUnsupportedPeripheralPreflight(err)) throw err;
                var status = Number(err && (err.status ?? err.statusCode ?? err.response?.status));
                this._renderPeripheralPreflight({
                    requiresConfirmation: false,
                    issues: [{ message: '设备固件未提供独立预检（HTTP ' + status + '），继续由保存接口执行最终校验。' }],
                    claims: []
                }, 'legacy');
                this._periphPreflightFingerprint = '';
                this._periphPreflightAcknowledgements = 0;
                return true;
            }
        },

        _collectI2cDriverConfig(data, type) {
            const prefix = type === 55 ? 'io-expander' : 'generic-sensor';
            const select = document.getElementById(prefix + '-driver');
            const name = select?.value || '';
            const meta = this._getPeripheralDriverMeta(type, name);
            if (!meta || select?.selectedOptions[0]?.disabled) return '请选择当前固件支持的驱动';
            data.driverName = String(meta.name).toUpperCase();
            data.driver = data.driverName;
            data.busPeripheralId = document.getElementById(prefix + '-bus-id')?.value || '';
            if (data.busPeripheralId) data.pins = '';
            else if (!/^\d+\s*,\s*\d+$/.test(data.pins || '')) return '请按 SDA、SCL 填写两个引脚，或选择已配置的 I2C 总线';
            const text = document.getElementById(prefix + '-address')?.value.trim() || '';
            const address = /^(?:0x[0-9a-f]+|\d+)$/i.test(text) ? Number(text) : NaN;
            const min = meta.addressMin ?? 0x08;
            const max = meta.addressMax ?? 0x77;
            if (!Number.isInteger(address) || address < min || address > max) return 'I2C 地址必须在 ' + this._formatI2cAddress(min) + ' 至 ' + this._formatI2cAddress(max) + ' 之间';
            if (Array.isArray(meta.addresses) && meta.addresses.length && !meta.addresses.some((item) => Number(item) === address)) return '该驱动支持的 I2C 地址为 ' + meta.addresses.map((item) => this._formatI2cAddress(item)).join('、');
            data.address = address;
            data.i2cAddress = address;
            data.frequency = type === 38 ? Number(this._periphFormConfig?.params?.frequency || 100000) : 100000;
            const interval = Number(document.getElementById(prefix + '-interval')?.value);
            const minInterval = Math.max(type === 55 ? 20 : 100, Number(meta.minIntervalMs) || 0);
            if (!Number.isInteger(interval) || interval < minInterval || interval > (type === 55 ? 1000 : 86400000)) return '采样间隔不能小于 ' + minInterval + ' ms，且需在页面标注范围内';
            data.sampleInterval = interval;
            if (type === 38) {
                const selectedChannel = document.getElementById('generic-sensor-channel')?.value || '';
                if (!(meta.channels || []).some((channel) => channel.name === selectedChannel)) return '请选择该驱动支持的测量通道';
                for (const param of meta.params || []) {
                    if (param.key === 'mode' && data.driver === 'MPU6050') continue;
                    const input = document.getElementById('generic-driver-' + param.key);
                    const value = input?.value.trim() === '' ? NaN : Number(input?.value);
                    const integer = ['integer', 'uint', 'int'].indexOf(param.type) >= 0;
                    const choices = param.choices || param.values;
                    if (!Number.isFinite(value) || (integer && !Number.isInteger(value)) ||
                        (param.min !== undefined && value < param.min) || (param.max !== undefined && value > param.max) ||
                        (Array.isArray(choices) && !choices.some((choice) => Number(typeof choice === 'object' ? choice.value : choice) === value))) {
                        return (input?.labels?.[0]?.textContent || param.label || param.key) + ' 不在驱动允许范围内';
                    }
                    data[param.key] = value;
                }
                if (data.driver === 'MPU6050') {
                    const channel = meta.channels.find((item) => item.name === selectedChannel);
                    data.mode = channel?.mode ? Number(channel.mode) - 1 : Number(this._periphFormConfig?.params?.mode || 0);
                }
                if (data.driver === 'INA226' && 2 * data.conversionTimeUs * data.averaging > 500000) {
                    return 'INA226 总转换时间不能超过 500 ms，请减少平均次数或转换时间';
                }
            }
            return '';
        },

        async savePeripheralConfig() {
            if (!this.guardDeveloperModeAction()) return;
            if (this._periphSaveBusy) return;
            const session = this._periphFormSession;
            const active = () => session && this._periphFormSession === session && this.currentPage === 'peripheral';
            if (!active()) return;
            const originalId = document.getElementById('peripheral-original-id').value;
            const id = document.getElementById('peripheral-id-input').value.trim();
            const name = document.getElementById('peripheral-name-input').value.trim();
            const type = document.getElementById('peripheral-type-input').value;
            const enabled = document.getElementById('peripheral-enabled-input').checked ? 1 : 0;
            const pinsInput = document.getElementById('peripheral-pins-input');
            const pinsStr = pinsInput.disabled ? '' : pinsInput.value.trim();
            const errEl = document.getElementById('peripheral-error');
            const typeNum = parseInt(type, 10);
            if (!this._isPeripheralTypeAvailable(typeNum)) {
                const message = Array.isArray(this._periphTypeCatalog)
                    ? t('peri-type-unavailable', '该外设类型当前不可用')
                    : t('peri-types-unavailable', '设备外设能力尚未加载，请稍后重试');
                this.showInlineError(errEl, message);
                if (!this._periphTypeCatalogLoading) this._loadPeripheralHardwareCapabilities();
                return;
            }
            if (!name || !type || (!pinsStr && !pinsInput.disabled && typeNum !== 60 && typeNum !== 51 && typeNum !== 38 && typeNum !== 55 && typeNum !== 56)) {
                this.showInlineError(errEl, t('peri-validate-empty', '请填写外设ID和名称'));
                return;
            }
            this.clearInlineError(errEl);
            const isEdit = originalId !== '';
            const finalId = isEdit ? originalId : (id || undefined);
            const data = { id: finalId, name: name, type: type, enabled: enabled, pins: pinsStr };
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
                    data.pairedInputId = (document.getElementById('gpio-paired-input-id')?.value || '').trim();
                    data.pairDebounceMs = document.getElementById('gpio-pair-debounce-ms')?.value || '200';
                }
                if (typeNum === 11 || typeNum === 12) {
                    data.ioSource = document.getElementById('gpio-io-source')?.value || 'gpio';
                    data.providerId = data.ioSource === 'expander' ? document.getElementById('gpio-provider-id')?.value || '' : '';
                    data.channel = data.ioSource === 'expander' ? Number(document.getElementById('gpio-provider-channel')?.value) : 0;
                    if (data.ioSource === 'expander') {
                        if (!data.providerId || !Number.isInteger(data.channel) || data.channel < 0 || document.getElementById('gpio-provider-channel')?.selectedOptions[0]?.disabled || document.getElementById('gpio-io-source')?.selectedOptions[0]?.disabled) {
                            this.showInlineError(errEl, '请选择固件支持的父扩展器和逻辑通道');
                            return;
                        }
                        data.pins = '';
                        data.inputMode = 0;
                        data.invertLogic = !!document.getElementById('gpio-invert-logic')?.checked;
                        data.passive = false;
                    }
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
                data.calibratedVoltage = !!document.getElementById('adc-calibrated-voltage')?.checked;
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
                var driverSelect = document.getElementById('generic-sensor-driver');
                var driverName = String(driverSelect?.value || '').trim().toUpperCase();
                if (driverName && driverSelect?.selectedOptions[0]?.disabled) {
                    this.showInlineError(errEl, '当前固件未提供所选传感器驱动，请重新选择');
                    return;
                }
                data.driverName = driverName;
                data.driver = driverName;
                data.busPeripheralId = document.getElementById('generic-sensor-bus-id')?.value.trim() || '';
                if (driverName) {
                    const error = this._collectI2cDriverConfig(data, 38);
                    if (error) {
                        this.showInlineError(errEl, error);
                        return;
                    }
                }
            } else if (typeNum === 55) {
                const error = this._collectI2cDriverConfig(data, 55);
                if (error) {
                    this.showInlineError(errEl, error);
                    return;
                }
            } else if (typeNum === 56) {
                const displayDescriptor = this._getPeripheralTypeDescriptor(56) || {};
                const displayLimits = displayDescriptor.configLimits || {};
                const readDisplayInteger = function(id, fallback, min, max) {
                    const value = Number(document.getElementById(id)?.value);
                    if (!Number.isInteger(value)) return fallback;
                    return Math.min(max, Math.max(min, value));
                };
                data.pins = '';
                data.displayBackend = document.getElementById('display-backend')?.value || 'spi_tft';
                data.displayDriver = document.getElementById('display-driver')?.value || 'st7789';
                data.touchBackend = document.getElementById('display-touch-backend')?.value || 'none';
                const isRgb = data.displayBackend === 'rgb';
                data.width = readDisplayInteger('display-width', 320, isRgb ? 480 : 128, isRgb ? 480 : 320);
                data.height = readDisplayInteger('display-height', 240, isRgb ? 480 : 128, isRgb ? 480 : 320);
                data.rotation = readDisplayInteger('display-rotation', 0, displayLimits.rotationMin ?? 0, displayLimits.rotationMax ?? 3);
                data.colorDepth = readDisplayInteger('display-color-depth', 16, 16, 16);
                data.bufferLines = readDisplayInteger('display-buffer-lines', 20, displayLimits.bufferLinesMin ?? 1, displayLimits.bufferLinesMax ?? 64);
                data.brightness = readDisplayInteger('display-brightness', 70, displayLimits.brightnessMin ?? 0, displayLimits.brightnessMax ?? 100);
                data.spiFrequency = readDisplayInteger('display-spi-frequency', 40000000, displayLimits.spiFrequencyMin ?? 1000000, displayLimits.spiFrequencyMax ?? 80000000);
                data.i2cFrequency = readDisplayInteger('display-i2c-frequency', 400000, 10000, 1000000);
                data.mosi = readDisplayInteger('display-mosi', -1, -1, 63);
                data.sclk = readDisplayInteger('display-sclk', -1, -1, 63);
                data.cs = readDisplayInteger('display-cs', -1, -1, 63);
                data.dc = readDisplayInteger('display-dc', -1, -1, 63);
                data.rst = readDisplayInteger('display-rst', -1, -1, 63);
                data.backlight = readDisplayInteger('display-backlight', -1, -1, 63);
                data.touchSda = data.touchBackend === 'gt911' ? readDisplayInteger('display-touch-sda', -1, -1, 63) : -1;
                data.touchScl = data.touchBackend === 'gt911' ? readDisplayInteger('display-touch-scl', -1, -1, 63) : -1;
                data.touchInt = data.touchBackend === 'gt911' ? readDisplayInteger('display-touch-int', -1, -1, 63) : -1;
                data.touchRst = data.touchBackend === 'gt911' ? readDisplayInteger('display-touch-rst', -1, -1, 63) : -1;
                if (data.mosi < 0 || data.sclk < 0 || data.cs < 0 || (!isRgb && data.dc < 0)) {
                    this.showInlineError(errEl, '必须配置 MOSI、SCLK、CS；SPI TFT 还需 DC');
                    return;
                }
                if (data.touchBackend === 'gt911' && (data.touchSda < 0 || data.touchScl < 0)) {
                    this.showInlineError(errEl, 'GT911 必须配置 SDA 和 SCL 引脚');
                    return;
                }
                if (data.backlight >= 0) {
                    data.backlightChannel = readDisplayInteger('display-backlight-channel', 0, 0, 15);
                    data.backlightActiveHigh = document.getElementById('display-backlight-active-high')?.checked !== false;
                }
                if (isRgb) {
                    const inputs = document.querySelectorAll('#display-rgb-inputs input, #display-rgb-inputs select');
                    for (const input of inputs) {
                        const key = input.id.slice(8);
                        if (key === 'touchBuzzerDurationMs') {
                            const value = Number(input.value);
                            if (!Number.isInteger(value) || value < 20 || value > 150) {
                                this.showInlineError(errEl, '触摸提示音时长必须为 20-150 ms');
                                return;
                            }
                            data[key] = value;
                        } else if (input.type === 'checkbox') data[key] = input.checked;
                        else if (input.type === 'number') {
                            const value = Number(input.value);
                            const isPin = /^(rgbData\d+|rgbDe|rgbVsync|rgbHsync|rgbPclk|controlSda|controlScl)$/.test(key);
                            const min = isPin ? -1 : (key === 'rgbFrequency' ? 1000000 : (/PulseWidth$/.test(key) ? 1 : 0));
                            const max = isPin ? 63 : (key === 'rgbFrequency' ? 16000000 : (key === 'controlAddress' ? 127 : (/Mask$/.test(key) ? 255 : 1023)));
                            if (!input.value.trim() || !Number.isInteger(value) || value < min || value > max) {
                                this.showInlineError(errEl, 'RGB 参数无效：' + key + ' (' + min + '-' + max + ')');
                                return;
                            }
                            data[key] = value;
                        } else data[key] = input.value;
                    }
                    if (Array.from({ length: 16 }, (_, index) => data['rgbData' + index]).some(pin => pin < 0) ||
                        ['rgbDe', 'rgbVsync', 'rgbHsync', 'rgbPclk'].some(key => data[key] < 0)) {
                        this.showInlineError(errEl, 'RGB 必须配置 D0-D15、DE、VSYNC、HSYNC 和 PCLK');
                        return;
                    }
                    const controlMasks = ['controlResetMask', 'controlEnableMask', 'controlBacklightMask',
                        'controlBuzzerMask', 'controlInactiveMask', 'controlTouchIntMask'];
                    const masksOverlap = controlMasks.some((key, index) => controlMasks.slice(index + 1).some(other => data[key] & data[other]));
                    if (masksOverlap) {
                        this.showInlineError(errEl, '复位、使能、背光、蜂鸣器、低电平和触摸 INT 掩码必须互不重叠');
                        return;
                    }
                    if (data.controlBuzzerMask && data.controlBackend !== 'ch32v003') {
                        this.showInlineError(errEl, '蜂鸣器掩码仅支持 CH32V003 辅助控制器');
                        return;
                    }
                    if (data.controlTouchIntMask &&
                        (data.controlTouchIntMask & (data.controlTouchIntMask - 1))) {
                        this.showInlineError(errEl, 'GT911 INT 同步掩码必须为单一位');
                        return;
                    }
                }
            } else if (typeNum === 27) {
                data.defaultValue = document.getElementById('dac-default-value')?.value || '0';
            } else if (typeNum === 41) {
                const readServoInteger = function (id, fallback) {
                    const value = parseInt(document.getElementById(id)?.value ?? '', 10);
                    return Number.isFinite(value) ? value : fallback;
                };
                const servoFrequency = readServoInteger('servo-frequency', 50);
                const servoMinPulse = readServoInteger('servo-min-pulse', 500);
                const servoMaxPulse = readServoInteger('servo-max-pulse', 2500);
                const servoMinAngle = readServoInteger('servo-min-angle', 0);
                const servoMaxAngle = readServoInteger('servo-max-angle', 180);
                const servoInitialAngle = readServoInteger('servo-initial-angle', 90);
                if (servoFrequency < 40 || servoFrequency > 400) {
                    this.showInlineError(errEl, t('peri-servo-frequency-invalid', '舵机 PWM 频率必须在 40-400 Hz 之间'));
                    return;
                }
                if (servoMinPulse < 100 || servoMaxPulse > 5000 || servoMinPulse >= servoMaxPulse ||
                    servoMaxPulse >= (1000000 / servoFrequency)) {
                    this.showInlineError(errEl, t('peri-servo-pulse-invalid', '舵机脉宽需在 100-5000 us 内，最小值小于最大值，且最大值小于 PWM 周期'));
                    return;
                }
                if (servoMinAngle < 0 || servoMaxAngle > 360 || servoMinAngle >= servoMaxAngle ||
                    servoInitialAngle < servoMinAngle || servoInitialAngle > servoMaxAngle) {
                    this.showInlineError(errEl, t('peri-servo-angle-invalid', '舵机角度需在 0-360 度内，启动角度必须位于配置范围'));
                    return;
                }
                data.frequency = servoFrequency;
                data.minPulse = servoMinPulse;
                data.maxPulse = servoMaxPulse;
                data.minAngle = servoMinAngle;
                data.maxAngle = servoMaxAngle;
                data.initialAngle = servoInitialAngle;
                data.inverted = !!document.getElementById('servo-inverted')?.checked;
                data.releaseAfterMove = !!document.getElementById('servo-release-after-move')?.checked;
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
                data.resolution = document.getElementById('encoder-resolution')?.value || '20';
                data.useInterrupt = '1';
            } else if (typeNum === 37) {
                // SD卡参数
                data.interface = document.getElementById('sdcard-interface')?.value || '1';
                data.frequency = document.getElementById('sdcard-frequency')?.value || '20000000';
            } else if (typeNum === 50) {
                data.use24Hour = document.getElementById('ds1302-hour-mode')?.value === '1';
                data.trickleCharge = document.getElementById('ds1302-trickle-charge')?.value || '0';
            } else if (typeNum === 52) {
                var sizeParts = (document.getElementById('lcd1602-size')?.value || '16x2').split('x');
                data.cols = parseInt(sizeParts[0], 10) || 16;
                data.rows = parseInt(sizeParts[1], 10) || 2;
                var lcdAddressText = document.getElementById('lcd1602-address')?.value.trim() || '';
                var lcdAddress = /^0x/i.test(lcdAddressText) ? parseInt(lcdAddressText.slice(2), 16) : parseInt(lcdAddressText, 10);
                var validLcdAddress = lcdAddress === 0 || (lcdAddress >= 0x20 && lcdAddress <= 0x27) ||
                    (lcdAddress >= 0x38 && lcdAddress <= 0x3F);
                if (!Number.isInteger(lcdAddress) || !validLcdAddress) {
                    this.showInlineError(errEl, 'LCD1602 地址必须为 0、0x20-0x27 或 0x38-0x3F');
                    return;
                }
                data.address = lcdAddress;
                data.backlight = !!document.getElementById('lcd1602-backlight')?.checked;
            } else if (typeNum === 54) {
                data.frequency = 4000000;
            }
            const saveBtn = document.getElementById('save-peripheral-btn');
            const origText = saveBtn?.textContent;
            this._periphSaveBusy = true;
            if (saveBtn) { saveBtn.disabled = true; saveBtn.textContent = '预检中...'; }
            try {
                const allowed = await this._runPeripheralPreflight(data);
                if (!active()) { this._periphSaveBusy = false; return; }
                if (!allowed) {
                    this._periphSaveBusy = false;
                    this._updatePeripheralTypeAvailability(typeNum);
                    if (saveBtn) {
                        saveBtn.textContent = this._periphPreflightFingerprint
                            ? t('peri-confirm-save', '确认并保存') : origText;
                    }
                    return;
                }
            } catch (err) {
                this._periphSaveBusy = false;
                if (saveBtn) saveBtn.textContent = origText;
                this._updatePeripheralTypeAvailability(typeNum);
                if (active()) this.showInlineError(errEl, this._peripheralIoErrorMessage(err, '保存前资源预检失败'));
                return;
            }
            if (saveBtn) saveBtn.textContent = t('peri-saving', '保存中...');
            const url = isEdit ? '/api/peripherals/update' : '/api/peripherals';
            return apiPost(url, data)
                .then(res => {
                    if (res && res.success && typeof window.apiInvalidateCache === 'function') window.apiInvalidateCache('/api/peripherals');
                    if (!active()) return;
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
                    if (!active()) return;
                    console.error('Save peripheral failed:', err);
                    this.showInlineError(errEl, err?.data?.error || err?.data?.message || t('peri-save-fail', '保存失败，请检查连接'));
                })
                .finally(() => {
                    this._periphSaveBusy = false;
                    if (saveBtn) {
                        saveBtn.textContent = this._periphPreflightFingerprint
                            ? t('peri-confirm-save', '确认并保存') : t('peri-save', '保存');
                    }
                    const currentType = document.getElementById('peripheral-type-input');
                    if (currentType) this._updatePeripheralTypeAvailability(Number(currentType.value));
                });
        },

    });
})();
