/**
 * 外设执行编辑器 - 传感器扩展
 * 仅在配置“传感器数据读取”动作时按需加载。
 */
(function() {
    var DIGITAL_SENSORS = ['dht11', 'dht22', 'ds18b20', 'sht30', 'sht31', 'aht20', 'bh1750', 'bmp280', 'mpu6050'];
    var PANEL_DEFAULT_FIELDS = {
        analog: 'voltage', radar: 'presence', dht11: 'temperature', dht22: 'temperature', ds18b20: 'temperature',
        sht30: 'temperature', sht31: 'temperature', aht20: 'temperature', ultrasonic: 'distance', current: 'current',
        current_loop: 'value', '4-20ma': 'value', '4_20ma': 'value', voltage: 'voltage', ntc: 'temperature',
        bh1750: 'lux', bmp280: 'pressure', mpu6050: 'accel_x', rf: 'rssi', digital: 'state', pulse: 'count',
        frequency: 'frequency'
    };
    var DEFAULT_FIELDS = {
        analog: 'voltage', digital: 'value', pulse: 'count', frequency: 'frequency', radar: 'presence',
        dht11: 'temperature', dht22: 'temperature', ds18b20: 'temperature', sht30: 'temperature',
        sht31: 'temperature', aht20: 'temperature', ultrasonic: 'distance', current: 'current',
        current_loop: 'value', '4-20ma': 'value', '4_20ma': 'value', voltage: 'voltage', ntc: 'temperature',
        bh1750: 'illuminance', bmp280: 'pressure', mpu6050: 'accelX', rf: 'value'
    };

    AppState.registerModule('periph-exec-sensor', {
        _getPeriphExecSensorPanelState(data) {
            var sensorCfg = {sensorCategory: 'analog', periphId: '', scaleFactor: 1, offset: 0, decimalPlaces: 1, sensorLabel: '', unit: '', dataField: '', deviceIndex: 0};
            if (data && data.actionValue) {
                try { Object.assign(sensorCfg, JSON.parse(data.actionValue)); } catch (error) {}
            }
            var sensorCategory = String(sensorCfg.sensorCategory || '').toLowerCase();
            var isDigitalSensor = DIGITAL_SENSORS.indexOf(sensorCategory) >= 0;
            if (isDigitalSensor && (!data || !data.actionValue || sensorCfg.scaleFactor === 0.00080586)) {
                sensorCfg.scaleFactor = 1;
                sensorCfg.offset = 0;
            }
            if (!sensorCfg.dataField) sensorCfg.dataField = PANEL_DEFAULT_FIELDS[sensorCategory] || 'value';
            if (!sensorCfg.sensorLabel) {
                var labels = {temperature: '温度', humidity: '湿度', distance: '距离', current: '电流', voltage: '电压',
                    presence: '存在', lux: '光照', illuminance: '光照', pressure: '气压', accel_x: '加速度', accelX: '加速度',
                    rssi: '信号强度', state: '状态', count: '累计计数', delta: '增量', direction: '方向', frequency: '频率', rpm: '转速', value: '数值'};
                sensorCfg.sensorLabel = labels[sensorCfg.dataField] || sensorCfg.dataField;
            }
            if (!sensorCfg.unit) {
                var units = {temperature: '°C', humidity: '%RH', distance: 'cm', current: 'A', voltage: 'V', presence: '',
                    lux: 'lx', illuminance: 'lx', pressure: 'hPa', accel_x: 'g', accelX: 'g', rssi: 'dBm', state: '',
                    count: '脉冲', delta: '脉冲', direction: '', frequency: 'Hz', rpm: 'RPM', value: ''};
                sensorCfg.unit = units[sensorCfg.dataField] || '';
            }
            if (sensorCfg.decimalPlaces === undefined || sensorCfg.decimalPlaces === null) {
                sensorCfg.decimalPlaces = isDigitalSensor ? 1 : 2;
            }
            var baseKeys = {periphId: 1, sensorCategory: 1, scaleFactor: 1, offset: 1, decimalPlaces: 1, sensorLabel: 1,
                unit: 1, dataField: 1, channelField: 1, deviceIndex: 1, bCoefficient: 1, seriesResistor: 1, nominalResistance: 1,
                shuntResistance: 1, inputGain: 1, inputOffset: 1, loopMinMa: 1, loopMaxMa: 1, engineeringMin: 1,
                engineeringMax: 1, sampleCount: 1, mapToEngineering: 1};
            var extra = {};
            Object.keys(sensorCfg).forEach(function(key) { if (!baseKeys[key]) extra[key] = sensorCfg[key]; });
            return {
                config: sensorCfg,
                extraJson: Object.keys(extra).length ? JSON.stringify(extra) : '',
                isDigital: isDigitalSensor || !!sensorCfg.channelField,
                isCurrentLoop: sensorCategory === 'current_loop' || sensorCategory === '4-20ma' || sensorCategory === '4_20ma'
            };
        },

        _buildPeriphExecSensorPanel(data) {
            var state = this._getPeriphExecSensorPanelState(data || {});
            var sensorCfg = state.config;
            var selectedCategory = sensorCfg.channelField ? 'configured' : sensorCfg.sensorCategory;
            var selCat = function(value) {
                return String(selectedCategory || '').toLowerCase() === String(value).toLowerCase() ? ' selected' : '';
            };
            return '<div class="fb-form-group pe-sensor-group pe-span-all"><div class="pe-sensor-config-grid">' +
                '<div class="fb-form-group"><label>传感器类别</label><select class="pe-sensor-category">' +
                '<option value="configured"' + selCat('configured') + '>已配置 I2C 传感器</option>' +
                '<option value="analog"' + selCat('analog') + '>模拟量 (ADC)</option>' +
                '<option value="digital"' + selCat('digital') + '>数字量 (DI)</option>' +
                '<option value="pulse"' + selCat('pulse') + '>编码器计数</option>' +
                '<option value="frequency"' + selCat('frequency') + '>编码器频率/转速</option>' +
                '<option value="dht11"' + selCat('dht11') + '>DHT11 温湿度</option>' +
                '<option value="dht22"' + selCat('dht22') + '>DHT22 温湿度</option>' +
                '<option value="ds18b20"' + selCat('ds18b20') + '>DS18B20 温度</option>' +
                '<option value="ultrasonic"' + selCat('ultrasonic') + '>HC-SR04 超声波</option>' +
                '<option value="radar"' + selCat('radar') + '>雷达存在检测</option>' +
                '<option value="rf"' + selCat('rf') + '>射频接收电平</option>' +
                '<option value="current"' + selCat('current') + '>电流型传感器</option>' +
                '<option value="current_loop"' + selCat('current_loop') + '>4-20mA 电流环</option>' +
                '<option value="voltage"' + selCat('voltage') + '>电压型传感器</option>' +
                '<option value="ntc"' + selCat('ntc') + '>NTC 热敏电阻</option>' +
                '<option value="sht30"' + selCat('sht30') + '>SHT30 温湿度</option>' +
                '<option value="SHT31"' + selCat('SHT31') + '>SHT31 温湿度</option>' +
                '<option value="AHT20"' + selCat('AHT20') + '>AHT20 温湿度</option>' +
                '<option value="BH1750"' + selCat('BH1750') + '>BH1750 光照</option>' +
                '<option value="BMP280"' + selCat('BMP280') + '>BMP280 气压</option>' +
                '<option value="MPU6050"' + selCat('MPU6050') + '>MPU6050 姿态</option></select></div>' +
                '<div class="fb-form-group pe-sensor-datafield-group"><label>数据字段</label><select class="pe-sensor-datafield">' +
                this._renderSensorDataFieldOptions(sensorCfg.sensorCategory, sensorCfg.dataField) + '</select></div>' +
                '<div class="fb-form-group pe-sensor-channel-group' + this._hiddenClass(!!sensorCfg.channelField) + '"><label>测量通道</label><select class="pe-sensor-channel-field"><option value="' + escapeHtml(sensorCfg.channelField || '') + '">' + escapeHtml(sensorCfg.channelField || '-- 选择通道 --') + '</option></select></div>' +
                '<div class="fb-form-group pe-sensor-property-group' + this._hiddenClass(!!sensorCfg.channelField) + '"><label>物模型字段</label><input class="pe-sensor-property" type="text" maxlength="63" value="' + escapeHtml(sensorCfg.channelField ? sensorCfg.dataField : '') + '" placeholder="例如 batteryVoltage"><small class="pe-help-text">填写上报数据的字段名；测量单位由已配置驱动提供。</small></div>' +
                '<small class="pe-help-text pe-sensor-configured-status pe-span-all' + this._hiddenClass(!!sensorCfg.channelField) + '"></small>' +
                '<div class="fb-form-group pe-sensor-devindex-group' + this._hiddenClass(String(sensorCfg.sensorCategory).toLowerCase() === 'ds18b20') + '"><label>设备索引</label>' +
                '<input type="number" class="pe-sensor-devindex" min="0" max="15" value="' + sensorCfg.deviceIndex + '"></div>' +
                '<div class="fb-form-group pe-sensor-ntc-group pe-span-all' + this._hiddenClass(String(sensorCfg.sensorCategory).toLowerCase() === 'ntc') + '">' +
                '<div class="fb-form-group"><label>NTC B值</label><input type="number" class="pe-sensor-ntc-b" step="any" value="' + (sensorCfg.bCoefficient || 3950) + '"></div>' +
                '<div class="fb-form-group"><label>串联电阻(Ω)</label><input type="number" class="pe-sensor-ntc-series" step="any" value="' + (sensorCfg.seriesResistor || 10000) + '"></div>' +
                '<div class="fb-form-group"><label>标称电阻(Ω)</label><input type="number" class="pe-sensor-ntc-nominal" step="any" value="' + (sensorCfg.nominalResistance || 10000) + '"></div>' +
                '<small class="pe-help-text">电路: VCC→串联电阻→ADC引脚→NTC→GND。常用 10K/B3950</small></div>' +
                '<div class="fb-form-group pe-sensor-current-loop-group pe-span-all' + this._hiddenClass(state.isCurrentLoop) + '"><div class="pe-sensor-calibration-grid">' +
                '<div class="fb-form-group"><label>分流电阻(Ω)</label><input type="number" class="pe-sensor-loop-shunt" min="0.1" step="any" value="' + (sensorCfg.shuntResistance !== undefined ? sensorCfg.shuntResistance : 165) + '"></div>' +
                '<div class="fb-form-group"><label>输入增益</label><input type="number" class="pe-sensor-loop-gain" min="0.001" step="any" value="' + (sensorCfg.inputGain !== undefined ? sensorCfg.inputGain : 1) + '"></div>' +
                '<div class="fb-form-group"><label>输入偏移(V)</label><input type="number" class="pe-sensor-loop-offset" step="any" value="' + (sensorCfg.inputOffset !== undefined ? sensorCfg.inputOffset : 0) + '"></div>' +
                '<div class="fb-form-group"><label>回路下限(mA)</label><input type="number" class="pe-sensor-loop-min" min="0" step="any" value="' + (sensorCfg.loopMinMa !== undefined ? sensorCfg.loopMinMa : 4) + '"></div>' +
                '<div class="fb-form-group"><label>回路上限(mA)</label><input type="number" class="pe-sensor-loop-max" min="0" step="any" value="' + (sensorCfg.loopMaxMa !== undefined ? sensorCfg.loopMaxMa : 20) + '"></div>' +
                '<div class="fb-form-group"><label>工程量下限</label><input type="number" class="pe-sensor-loop-eng-min" step="any" value="' + (sensorCfg.engineeringMin !== undefined ? sensorCfg.engineeringMin : 0) + '"></div>' +
                '<div class="fb-form-group"><label>工程量上限</label><input type="number" class="pe-sensor-loop-eng-max" step="any" value="' + (sensorCfg.engineeringMax !== undefined ? sensorCfg.engineeringMax : 100) + '"></div>' +
                '<div class="fb-form-group"><label>采样次数</label><input type="number" class="pe-sensor-loop-samples" min="1" max="32" value="' + (sensorCfg.sampleCount !== undefined ? sensorCfg.sampleCount : 8) + '"></div></div>' +
                '<label class="fb-checkbox fb-m-0"><input type="checkbox" class="pe-sensor-loop-map"' + (sensorCfg.mapToEngineering ? ' checked' : '') + '><span>映射到工程量范围</span></label>' +
                '<small class="pe-help-text">默认返回 mA；启用映射后按 4-20mA 线性换算到工程量。分流电阻和寄存器范围请按实际硬件校准。</small></div>' +
                '<div class="fb-form-group pe-sensor-calibration-group' + this._hiddenClass(!state.isDigital) + '">' +
                '<div class="fb-form-group"><label>缩放系数</label><input type="number" class="pe-sensor-scale" step="any" value="' + sensorCfg.scaleFactor + '"></div>' +
                '<div class="fb-form-group"><label>偏移量</label><input type="number" class="pe-sensor-offset" step="any" value="' + sensorCfg.offset + '"></div></div>' +
                '<div class="fb-form-group"><label>小数位数</label><input type="number" class="pe-sensor-decimals" min="0" max="6" value="' + sensorCfg.decimalPlaces + '"></div>' +
                '<div class="fb-form-group"><label>数据标签</label><input type="text" class="pe-sensor-label" value="' + escapeHtml(sensorCfg.sensorLabel) + '" placeholder="数据标签"></div>' +
                '<div class="fb-form-group"><label>单位</label><input type="text" class="pe-sensor-unit" value="' + escapeHtml(sensorCfg.unit) + '" maxlength="8" placeholder="°C, %, V..."></div>' +
                '<div class="fb-form-group pe-sensor-advanced-group pe-span-all' + this._hiddenClass(!state.isDigital) + '"><label>高级参数(JSON)</label>' +
                '<textarea class="pe-sensor-extra-json" rows="2" maxlength="512">' + escapeHtml(state.extraJson) + '</textarea>' +
                '<small class="pe-help-text">' + (state.isDigital ? '数字传感器无需高级参数' : '可填写分压比、电流零点、I2C 地址等高级参数；留空则使用默认值。') + '</small></div></div></div>';
        },

        _getPeriphExecLocalSensorSources() {
            var sources = [];
            var seen = {};
            function addSource(id, label) {
                if (!id || seen[id]) return;
                seen[id] = true;
                sources.push({id: id, label: label});
            }
            (this._peSensorSources || []).forEach(function(source) {
                if (source && source.id) addSource(String(source.id), source.label || source.id);
            });
            (this._pePeripherals || []).forEach(function(periph) {
                if (!periph || periph.enabled === false || Number(periph.type) !== 1) return;
                addSource(String(periph.id), (periph.name || periph.id) + ' / UART');
            });
            return sources;
        },

        _renderSensorDataFieldOptions(category, selectedValue) {
            var cat = String(category || '').toLowerCase();
            var fieldsByCategory = {
                analog: [['voltage', '电压'], ['value', '原始值']], digital: [['value', '数值']],
                pulse: [['count', '累计计数'], ['delta', '本次增量'], ['direction', '方向 (-1/0/1)']],
                frequency: [['frequency', '频率 (Hz)'], ['rpm', '转速 (RPM)']], radar: [['presence', '存在检测']],
                rf: [['value', '接收电平']], rflevel: [['value', '接收电平']],
                dht11: [['temperature', '温度'], ['humidity', '湿度']], dht22: [['temperature', '温度'], ['humidity', '湿度']],
                sht30: [['temperature', '温度'], ['humidity', '湿度']], sht31: [['temperature', '温度'], ['humidity', '湿度']],
                aht20: [['temperature', '温度'], ['humidity', '湿度']], ultrasonic: [['distance', '距离']],
                current: [['current', '电流']], current_loop: [['value', '回路值'], ['temperature', '温度'], ['humidity', '湿度']],
                '4-20ma': [['value', '回路值'], ['temperature', '温度'], ['humidity', '湿度']],
                '4_20ma': [['value', '回路值'], ['temperature', '温度'], ['humidity', '湿度']], voltage: [['voltage', '电压']],
                ntc: [['temperature', '温度']], bh1750: [['illuminance', '光照']],
                bmp280: [['temperature', '温度'], ['pressure', '气压'], ['altitude', '海拔']],
                mpu6050: [['accelX', 'X 加速度'], ['accelY', 'Y 加速度'], ['accelZ', 'Z 加速度'], ['temperature', '温度'], ['gyroX', 'X 角速度'], ['gyroY', 'Y 角速度'], ['gyroZ', 'Z 角速度']]
            };
            var fields = fieldsByCategory[cat] || [['temperature', '温度']];
            var selected = selectedValue || fields[0][0];
            return fields.map(function(item) {
                return '<option value="' + escapeHtml(item[0]) + '"' + (selected === item[0] ? ' selected' : '') + '>' + escapeHtml(item[1]) + '</option>';
            }).join('');
        },

        _populateSensorPeriphSelect(block, category, selectedValue) {
            var select = block.querySelector('.pe-target-periph');
            if (!select) return;
            var cat = String(category || '').toLowerCase();
            var analogTypes = [15, 26];
            var digitalTypes = [11, 13, 14];
            var allowedTypes;
            if (cat === 'configured') allowedTypes = [38];
            else if (cat === 'digital') allowedTypes = digitalTypes;
            else if (cat === 'pulse' || cat === 'frequency') allowedTypes = [43];
            else if (cat === 'radar') allowedTypes = [49];
            else if (cat === 'rf' || cat === 'rflevel') allowedTypes = [48];
            else if (cat === 'dht11' || cat === 'dht22') allowedTypes = [11, 13, 14, 38, 44];
            else if (cat === 'ds18b20') allowedTypes = [44, 38, 11, 13, 14];
            else if (cat === 'ultrasonic') allowedTypes = [38, 11, 13, 14];
            else if (cat === 'sht30' || cat === 'sht31' || cat === 'aht20' || cat === 'bh1750' || cat === 'bmp280' || cat === 'mpu6050') allowedTypes = [2, 38];
            else if (cat === 'current' || cat === 'voltage' || cat === 'ntc' || cat === 'current_loop' || cat === '4-20ma' || cat === '4_20ma') allowedTypes = analogTypes.concat([38]);
            else allowedTypes = analogTypes;
            var previous = selectedValue || select.value;
            select.innerHTML = '<option value="">--</option>';
            (this._pePeripherals || []).filter(function(periph) {
                return allowedTypes.indexOf(Number(periph.type)) >= 0 &&
                    (cat !== 'configured' || !!(periph.params?.driverName || periph.params?.driver || periph.driver));
            }).forEach(function(periph) {
                var option = document.createElement('option');
                option.value = periph.id;
                option.textContent = (periph.name || periph.id) + ((periph.pins && periph.pins.length) ? ' (Pin ' + periph.pins[0] + ')' : '') +
                    (periph.featureActive === false || periph.enabled === false ? ' · 未启用' : '');
                select.appendChild(option);
            });
            if (previous) {
                if (!Array.from(select.options).some(function(option) { return option.value === previous; })) {
                    var missing = document.createElement('option');
                    missing.value = previous;
                    missing.textContent = previous + ' · 外设不可用';
                    missing.disabled = true;
                    select.appendChild(missing);
                }
                select.value = previous;
            }
        },

        _getConfiguredSensorDetails(id) {
            var peripheral = (this._pePeripherals || []).find(function(item) { return item.id === id && Number(item.type) === 38; });
            var params = peripheral?.params || {};
            var name = params.driverName || params.driver || peripheral?.driver || '';
            var descriptor = (this._peTypeCatalog || []).find(function(item) { return Number(item.value ?? item.type) === 38; });
            var driver = (descriptor?.driverDescriptors || []).find(function(item) { return String(item.name).toLowerCase() === String(name).toLowerCase(); });
            var channels = (driver?.channels || []).filter(function(channel) {
                return !channel.mode || Number(channel.mode) === (Number(params.mode || 0) === 1 ? 2 : 1);
            });
            var reason = !peripheral ? '请选择已配置的 I2C 传感器' :
                peripheral.enabled === false ? '该外设已禁用' :
                peripheral.featureActive === false || descriptor?.featureActive === false ? (descriptor?.featureReason || 'I2C 传感器功能当前未启用') :
                !driver || !channels.length ? '当前固件未提供该驱动的可用通道' : '';
            return { peripheral: peripheral, driver: driver, channels: channels, reason: reason };
        },

        _syncConfiguredSensorAction(block, saved) {
            if (!block || block.querySelector('.pe-sensor-category')?.value !== 'configured') return;
            var target = block.querySelector('.pe-target-periph');
            if (saved?.periphId && target) target.value = saved.periphId;
            var details = this._getConfiguredSensorDetails(target?.value || '');
            var select = block.querySelector('.pe-sensor-channel-field');
            var selected = saved?.channelField || select?.value || details.peripheral?.params?.channel || '';
            if (select) {
                select.innerHTML = '';
                details.channels.forEach(function(channel) {
                    var option = document.createElement('option');
                    option.value = channel.name;
                    option.textContent = (channel.label || channel.name) + (channel.unit ? ' (' + channel.unit + ')' : '');
                    select.appendChild(option);
                });
                if (selected && !details.channels.some(function(channel) { return channel.name === selected; })) {
                    var unavailable = document.createElement('option');
                    unavailable.value = selected;
                    unavailable.textContent = selected + ' · 通道不可用';
                    unavailable.disabled = true;
                    select.appendChild(unavailable);
                }
                if (selected) select.value = selected;
            }
            var channel = details.channels.find(function(item) { return item.name === select?.value; });
            var unit = block.querySelector('.pe-sensor-unit');
            if (unit) { unit.value = channel?.unit || ''; unit.readOnly = true; }
            var property = block.querySelector('.pe-sensor-property');
            if (property && saved?.dataField) property.value = saved.dataField;
            if (property && !property.value && channel) property.value = channel.name;
            var status = block.querySelector('.pe-sensor-configured-status');
            if (status) status.textContent = details.reason || (!channel ? '请选择当前驱动支持的测量通道' : '使用 ' + details.driver.name + ' 的已保存接线与校准参数');
        },

        onSensorCategoryChange(selectEl, index, preserveValues) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            var cat = selectEl.value;
            var normCat = String(cat || '').toLowerCase();
            this._populateSensorPeriphSelect(block, cat);
            var fieldGroup = block.querySelector('.pe-sensor-datafield-group');
            this._setSectionVisible(fieldGroup, normCat !== 'configured');
            this._setSectionVisible(block.querySelector('.pe-sensor-channel-group'), normCat === 'configured');
            this._setSectionVisible(block.querySelector('.pe-sensor-property-group'), normCat === 'configured');
            this._setSectionVisible(block.querySelector('.pe-sensor-configured-status'), normCat === 'configured');
            var fieldSelect = block.querySelector('.pe-sensor-datafield');
            if (fieldSelect) fieldSelect.innerHTML = this._renderSensorDataFieldOptions(cat, preserveValues ? fieldSelect.value : '');
            var labelEl = block.querySelector('.pe-sensor-label');
            var unitEl = block.querySelector('.pe-sensor-unit');
            var scaleEl = block.querySelector('.pe-sensor-scale');
            var decEl = block.querySelector('.pe-sensor-decimals');
            if (unitEl) unitEl.readOnly = normCat === 'configured';
            if (!preserveValues) {
            if (normCat === 'analog') {
                if (labelEl && (!labelEl.value || labelEl.value === '温度' || labelEl.value === '湿度')) labelEl.value = '电压';
                if (unitEl && (!unitEl.value || unitEl.value === '℃' || unitEl.value === '%')) unitEl.value = 'V';
                if (scaleEl && (!scaleEl.value || scaleEl.value === '1')) scaleEl.value = '0.00080586';
                if (decEl && (!decEl.value || decEl.value === '1' || decEl.value === '0')) decEl.value = '2';
            } else if (normCat === 'pulse') {
                if (labelEl) labelEl.value = '编码器计数';
                if (unitEl) unitEl.value = '脉冲';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '0';
            } else if (normCat === 'frequency') {
                if (labelEl) labelEl.value = '编码器频率';
                if (unitEl) unitEl.value = 'Hz';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '2';
            } else if (normCat === 'current') {
                if (labelEl) labelEl.value = '电流';
                if (unitEl) unitEl.value = 'A';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '3';
            } else if (normCat === 'current_loop' || normCat === '4-20ma' || normCat === '4_20ma') {
                if (labelEl) labelEl.value = '4-20mA';
                if (unitEl) unitEl.value = 'mA';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '3';
            } else if (normCat === 'voltage') {
                if (labelEl) labelEl.value = '电压';
                if (unitEl) unitEl.value = 'V';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '2';
            } else if (normCat === 'ntc') {
                if (labelEl) labelEl.value = '温度';
                if (unitEl) unitEl.value = '℃';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '1';
            } else if (normCat === 'ultrasonic') {
                if (labelEl) labelEl.value = '距离';
                if (unitEl) unitEl.value = 'cm';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '1';
            } else if (normCat === 'radar') {
                if (labelEl) labelEl.value = '雷达存在';
                if (unitEl) unitEl.value = '';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '0';
            } else if (normCat === 'rf' || normCat === 'rflevel') {
                if (labelEl) labelEl.value = '射频电平';
                if (unitEl) unitEl.value = '';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '0';
            } else if (normCat === 'bh1750') {
                if (labelEl) labelEl.value = '光照';
                if (unitEl) unitEl.value = 'lx';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '0';
            } else if (normCat === 'sht30' || normCat === 'sht31' || normCat === 'aht20' ||
                normCat === 'dht11' || normCat === 'dht22' || normCat === 'ds18b20') {
                if (labelEl) labelEl.value = '温度';
                if (unitEl) unitEl.value = '℃';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '1';
            } else if (normCat === 'bmp280') {
                if (labelEl) labelEl.value = '气压/温度';
                if (unitEl) unitEl.value = '℃';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '1';
            } else if (normCat === 'mpu6050') {
                if (labelEl) labelEl.value = '加速度';
                if (unitEl) unitEl.value = 'm/s2';
                if (scaleEl) scaleEl.value = '1';
                if (decEl) decEl.value = '2';
            }
            }
            this._setSectionVisible(block.querySelector('.pe-sensor-devindex-group'), normCat === 'ds18b20');
            this._setSectionVisible(block.querySelector('.pe-sensor-ntc-group'), normCat === 'ntc');
            this._setSectionVisible(block.querySelector('.pe-sensor-current-loop-group'),
                normCat === 'current_loop' || normCat === '4-20ma' || normCat === '4_20ma');
            var isDigital = DIGITAL_SENSORS.indexOf(normCat) >= 0 || normCat === 'configured';
            this._setSectionVisible(block.querySelector('.pe-sensor-calibration-group'), !isDigital);
            var advanced = block.querySelector('.pe-sensor-advanced-group');
            this._setSectionVisible(advanced, !isDigital);
            var help = advanced && advanced.querySelector('.pe-help-text');
            if (help) help.textContent = isDigital ? '数字传感器（DHT/DS18B20等）无需高级参数' : '可填写分压比、电流零点、I2C 地址等高级参数；留空则使用默认值。';
            if (normCat === 'configured') this._syncConfiguredSensorAction(block);
        },
        _collectPeriphExecSensorAction(item, action) {
            var sensorCategory = item.querySelector('.pe-sensor-category')?.value || 'analog';
            var normalized = String(sensorCategory).toLowerCase();
            if (normalized === 'configured') {
                var configuredId = item.querySelector('.pe-target-periph')?.value || '';
                var details = this._getConfiguredSensorDetails(configuredId);
                var channelField = item.querySelector('.pe-sensor-channel-field')?.value || '';
                var channel = details.channels.find(function(entry) { return entry.name === channelField; });
                var property = item.querySelector('.pe-sensor-property')?.value?.trim() || '';
                if (details.reason || !channel || !/^[A-Za-z_][A-Za-z0-9_]{0,62}$/.test(property)) {
                    this._peConfiguredSensorError = details.reason || (!channel ? '请选择当前驱动支持的测量通道' : '物模型字段须以字母或下划线开头，最多 63 个字母、数字或下划线');
                }
                action.actionValue = JSON.stringify({
                    periphId: configuredId, sensorCategory: details.driver?.name || '',
                    channelField: channelField, dataField: property, scaleFactor: 1, offset: 0,
                    decimalPlaces: Math.max(0, Math.min(6, Number(item.querySelector('.pe-sensor-decimals')?.value || 0))),
                    sensorLabel: item.querySelector('.pe-sensor-label')?.value?.trim() || channelField,
                    unit: channel?.unit || ''
                });
                return true;
            }
            var scaleFactor = parseFloat(item.querySelector('.pe-sensor-scale')?.value);
            var offset = parseFloat(item.querySelector('.pe-sensor-offset')?.value);
            if (isNaN(scaleFactor)) scaleFactor = 1;
            if (isNaN(offset)) offset = 0;
            if (DIGITAL_SENSORS.indexOf(normalized) >= 0) {
                scaleFactor = 1;
                offset = 0;
            }
            var decimalPlaces = parseInt(item.querySelector('.pe-sensor-decimals')?.value);
            if (isNaN(decimalPlaces)) decimalPlaces = 2;
            var sensorData = {
                periphId: item.querySelector('.pe-target-periph')?.value || '',
                sensorCategory: sensorCategory,
                scaleFactor: scaleFactor,
                offset: offset,
                decimalPlaces: decimalPlaces,
                sensorLabel: item.querySelector('.pe-sensor-label')?.value?.trim() || '',
                unit: item.querySelector('.pe-sensor-unit')?.value?.trim() || '',
                dataField: item.querySelector('.pe-sensor-datafield')?.value || DEFAULT_FIELDS[normalized] || 'value'
            };
            if (normalized === 'ds18b20') sensorData.deviceIndex = parseInt(item.querySelector('.pe-sensor-devindex')?.value) || 0;
            if (normalized === 'ntc') {
                var ntcB = parseFloat(item.querySelector('.pe-sensor-ntc-b')?.value);
                var ntcSeries = parseFloat(item.querySelector('.pe-sensor-ntc-series')?.value);
                var ntcNominal = parseFloat(item.querySelector('.pe-sensor-ntc-nominal')?.value);
                sensorData.bCoefficient = isNaN(ntcB) ? 3950 : ntcB;
                sensorData.seriesResistor = isNaN(ntcSeries) ? 10000 : ntcSeries;
                sensorData.nominalResistance = isNaN(ntcNominal) ? 10000 : ntcNominal;
            }
            if (normalized === 'current_loop' || normalized === '4-20ma' || normalized === '4_20ma') {
                var readFloat = function(selector, fallback) {
                    var value = parseFloat(item.querySelector(selector)?.value);
                    return isNaN(value) ? fallback : value;
                };
                sensorData.shuntResistance = readFloat('.pe-sensor-loop-shunt', 165);
                sensorData.inputGain = readFloat('.pe-sensor-loop-gain', 1);
                sensorData.inputOffset = readFloat('.pe-sensor-loop-offset', 0);
                sensorData.loopMinMa = readFloat('.pe-sensor-loop-min', 4);
                sensorData.loopMaxMa = readFloat('.pe-sensor-loop-max', 20);
                sensorData.engineeringMin = readFloat('.pe-sensor-loop-eng-min', 0);
                sensorData.engineeringMax = readFloat('.pe-sensor-loop-eng-max', 100);
                var samples = parseInt(item.querySelector('.pe-sensor-loop-samples')?.value || '8', 10);
                sensorData.sampleCount = Math.max(1, Math.min(32, isNaN(samples) ? 8 : samples));
                sensorData.mapToEngineering = !!item.querySelector('.pe-sensor-loop-map')?.checked;
            }
            var extraJson = item.querySelector('.pe-sensor-extra-json')?.value?.trim() || '';
            if (extraJson) {
                try {
                    var extra = JSON.parse(extraJson);
                    if (extra && typeof extra === 'object' && !Array.isArray(extra)) {
                        Object.keys(extra).forEach(function(key) { sensorData[key] = extra[key]; });
                    } else {
                        this._peSensorExtraJsonError = true;
                    }
                } catch (error) {
                    this._peSensorExtraJsonError = true;
                }
            }
            action.actionValue = JSON.stringify(sensorData);
            return true;
        }
    });
})();
