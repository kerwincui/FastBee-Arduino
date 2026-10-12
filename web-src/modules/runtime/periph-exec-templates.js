/** Editor drafts, deferred action panels and read-only save preflight. */
(function() {
    // Materialize the static, trusted wizard only in a new-rule editor.
const wizardMarkup = "<summary>从只读采集模板开始（新增规则）</summary><p class=\"u-help\">1. 选择已配置外设与字段；2. 预览；3. 填入禁用草稿。不会自动保存、启用或执行。</p><div class=\"config-form-grid\"><div class=\"fb-form-group\"><label for=\"pe-template-kind\">采集模板</label><select id=\"pe-template-kind\"><option value=\"\">-- 选择模板 --</option><option value=\"configured-sensor\">已配置 I2C 传感器通道 → 定时上报</option><option value=\"rtc-read\">DS1302 只读日历 → 定时上报</option><option value=\"sd-status\">SD 状态快照 → 定时文本上报</option><option value=\"configured-modbus\">已核对 Modbus 只读任务 → 定时上报</option></select></div><div class=\"fb-form-group\"><label for=\"pe-template-target\">实际外设 / 已保存任务</label><select id=\"pe-template-target\"></select></div><div id=\"pe-template-channel-group\" class=\"fb-form-group is-hidden\"><label for=\"pe-template-channel\">驱动测量通道</label><select id=\"pe-template-channel\"></select></div><div id=\"pe-template-property-group\" class=\"fb-form-group is-hidden\"><label for=\"pe-template-property\">平台物模型字段</label><input id=\"pe-template-property\" type=\"text\" maxlength=\"63\" placeholder=\"按实际物模型填写\"></div><div class=\"fb-form-group\"><label for=\"pe-template-interval\">采集间隔（秒）</label><input id=\"pe-template-interval\" type=\"number\" min=\"5\" max=\"86400\" step=\"1\" value=\"60\"></div></div><div id=\"pe-template-modbus-group\" class=\"u-help is-hidden\"><p>仅引用已保存的 FC03/FC04、1～64 个寄存器、有字段映射的任务；不配置串口，不生成控制设备或写命令。摘要不含完整倍率/字序，请先到协议配置核对。任务仍使用原索引，重排/替换任务后必须重新检查规则。</p><label class=\"fb-checkbox\"><input id=\"pe-template-modbus-reviewed\" type=\"checkbox\"><span>我已根据确切仪表型号/版本核对地址、功能码、寄存器、字序、倍率和单位</span></label></div><button id=\"pe-template-review\" class=\"fb-btn fb-btn-sm\" type=\"button\" disabled>预览草稿</button><p id=\"pe-template-preview\" class=\"u-help\" role=\"status\" aria-live=\"polite\"></p><label class=\"fb-checkbox\"><input id=\"pe-template-confirm\" type=\"checkbox\"><span>确认替换当前未保存的规则表单</span></label><button id=\"pe-template-apply\" class=\"fb-btn fb-btn-sm\" type=\"button\" disabled>填入禁用草稿</button>";
    const field = name => document.getElementById('pe-template-' + name);
    const utf8Size = text => Array.from(text).reduce((size, character) => {
        const point = character.codePointAt(0);
        return size + (point <= 0x7f ? 1 : point <= 0x7ff ? 2 : point <= 0xffff ? 3 : 4);
    }, 0);
    const draftName = text => {
        let prefix = '';
        for (const character of String(text)) {
            if (utf8Size(prefix + character) > 24) break;
            prefix += character;
        }
        // Keep the full name below the firmware's 48-byte limit, including multibyte labels.
        return prefix + ' 定时只读采集';
    };
    const choices = (select, rows, prompt) => {
        select.replaceChildren();
        const empty = document.createElement('option');
        empty.value = ''; empty.textContent = prompt; select.appendChild(empty);
        rows.forEach(row => {
            const option = document.createElement('option');
            option.value = row.id; option.textContent = row.label; select.appendChild(option);
        });
    };
    Object.assign(AppState, {
        _sdRecorderConfigSchema() {
            const descriptor = (this._peTypeCatalog || []).find(p => Number(p.value ?? p.type) === 37)?.sdRecorderConfig;
            const keys = ['sampleSec', 'maxAgeSec', 'flushSec', 'fileKiB', 'fileCount'];
            if (descriptor?.schema !== 1 || !Number.isInteger(descriptor.sourceMaxBytes) || descriptor.sourceMaxBytes < 1 ||
                !Array.isArray(descriptor.numbers) || descriptor.numbers.length !== keys.length) return null;
            const schema = {sourceMaxBytes: descriptor.sourceMaxBytes};
            for (const field of descriptor.numbers) {
                if (!field || !keys.includes(field.key) || schema[field.key] || field.integer !== true ||
                    ![field.min, field.max, field.defaultValue].every(Number.isInteger) ||
                    field.min > field.defaultValue || field.defaultValue > field.max) return null;
                schema[field.key] = field;
            }
            return schema;
        },

        _pca9685CommandCatalog() {
            const type = (this._peTypeCatalog || []).find(item => Number(item.value ?? item.type) === 57);
            const labels = {duty: '占空比', pulse: '脉宽', angle: '角度', enable: '启用输出', disable: '禁用输出', stop: '停止并释放'};
            if (type?.commandSchemaVersion !== 1 || !Array.isArray(type.commandDescriptors)) return [];
            return type.commandDescriptors.filter(item => Object.prototype.hasOwnProperty.call(labels, item.id) && Array.isArray(item.fields))
                .map(item => ({...item, label: labels[item.id]}));
        },

        _pca9685CommandError(action) {
            if (Number(action.actionType) !== 10 || !(this._pePeripherals || []).some(p =>
                p.id === action.targetPeriphId && Number(p.type) === 57)) return '';
            const entries = this._pca9685CommandCatalog();
            if (!entries.length) return '固件未提供 PCA9685 动作参数描述，请刷新并使用配套固件';
            if (action.useReceivedValue) return 'PCA9685 仅允许显式字面参数，不支持接收值替换';
            let command;
            try { command = JSON.parse(action.actionValue); } catch (_) { return 'PCA9685 动作必须是 JSON 对象'; }
            if (!command || Array.isArray(command) || typeof command !== 'object' ||
                new TextEncoder().encode(action.actionValue).length > 256) return 'PCA9685 动作载荷无效';
            const descriptor = entries.find(item => item.id === command.action);
            if (!descriptor || Object.keys(command).some(key => key !== 'action' &&
                !descriptor.fields.some(field => field.key === key))) return '请选择支持的 PCA9685 字面动作，不支持初始化或附加参数';
            for (const field of descriptor.fields) {
                const value = command[field.key];
                if (!['channel', 'value'].includes(field.key) || !['integer', 'number'].includes(field.type) ||
                    typeof value !== 'number' || !Number.isFinite(value) ||
                    (field.type === 'integer' && !Number.isInteger(value)) ||
                    (field.min !== undefined && value < field.min) ||
                    (field.exclusiveMin !== undefined && value <= field.exclusiveMin) ||
                    (field.max !== undefined && value > field.max)) return 'PCA9685 参数不符合固件描述：' + field.key;
            }
            return '';
        },

        _syncPca9685ActionFields(block) {
            if (!block) return;
            const id = block.querySelector('.pe-target-periph')?.value;
            const active = Number(block.querySelector('.pe-action-type')?.value) === 10 &&
                (this._pePeripherals || []).some(p => p.id === id && Number(p.type) === 57);
            let group = block.querySelector('.pe-pca9685-group');
            if (!active) { if (group) this._setSectionVisible(group, false); return; }
            const raw = block.querySelector('.pe-action-value');
            if (!raw) return;
            const descriptors = this._pca9685CommandCatalog();
            if (!group) {
                group = document.createElement('div'); group.className = 'fb-form-group pe-pca9685-group';
                group.innerHTML = '<label>PCA9685 会话动作</label><select class="pe-pca-command"></select>' +
                    '<div class="pe-pca-channel-fields"><label>通道<input class="pe-pca-channel" type="number"></label>' +
                    '<label>设置值 <span class="pe-pca-unit"></span><input class="pe-pca-value" type="number"></label></div>' +
                    '<small class="pe-pca-schema-error"></small>' +
                    '<small class="pe-help-text">先在外设诊断中完成本次上电确认与初始化。规则不自动初始化，禁止接收值替换；动作接受不表示实物测量。</small>';
                block.querySelector('.pe-action-value-group').insertAdjacentElement('afterend', group);
                const update = () => {
                    const action = group.querySelector('.pe-pca-command').value;
                    const descriptor = this._pca9685CommandCatalog().find(item => item.id === action);
                    const command = {action};
                    for (const field of descriptor?.fields || []) {
                        const input = group.querySelector('.pe-pca-' + field.key);
                        const text = input?.value.trim() || '';
                        command[field.key] = text ? Number(text) : null;
                    }
                    raw.value = JSON.stringify(command);
                    delete group.dataset.raw;
                    this._syncPca9685ActionFields(block);
                };
                group.querySelectorAll('input,select').forEach(el => el.addEventListener('input', update));
            }
            const schema = JSON.stringify(descriptors);
            if (group.dataset.schema !== schema) {
                group.querySelector('.pe-pca-command').innerHTML = '<option value="">-- 选择 --</option>' + descriptors.map(item =>
                    '<option value="' + escapeHtml(item.id) + '">' + escapeHtml(item.label) + '</option>').join('');
                group.dataset.schema = schema;
                delete group.dataset.raw;
            }
            group.querySelector('.pe-pca-schema-error').textContent = descriptors.length ? '' : '固件未提供动作参数描述，当前配置保留，暂不能保存';
            group.querySelectorAll('input,select').forEach(input => { input.disabled = !descriptors.length; });
            if (group.dataset.raw !== raw.value) {
                let command = {};
                try { command = JSON.parse(raw.value || '{}') || {}; } catch (err) {}
                const descriptor = descriptors.find(item => item.id === command.action);
                group.querySelector('.pe-pca-command').value = descriptor ? command.action : '';
                group.querySelector('.pe-pca-channel').value = Number.isInteger(command.channel) ? command.channel : '';
                group.querySelector('.pe-pca-value').value = Number.isFinite(command.value) ? command.value : '';
                for (const key of ['channel', 'value']) {
                    const input = group.querySelector('.pe-pca-' + key);
                    const field = descriptor?.fields.find(item => item.key === key);
                    for (const attr of ['min', 'max', 'step']) input.removeAttribute(attr);
                    input.required = field?.required === true;
                    if (field?.min !== undefined) input.min = String(field.min);
                    if (field?.exclusiveMin !== undefined) input.min = String(field.exclusiveMin);
                    if (field?.max !== undefined) input.max = String(field.max);
                    input.step = field?.type === 'integer' ? '1' : 'any';
                }
                group.querySelector('.pe-pca-unit').textContent = descriptor?.fields.find(field => field.key === 'value')?.unit || '';
                this._setSectionVisible(group.querySelector('.pe-pca-channel-fields'), !!descriptor?.fields.length);
                group.dataset.raw = raw.value;
            }
            this._setSectionVisible(group, true);
            this._setSectionVisible(block.querySelector('.pe-action-value-group'), false);
            this._setSectionVisible(block.querySelector('.pe-use-received-value-group'), false);
            const received = block.querySelector('.pe-use-received-value');
            if (received) received.checked = false;
        },

        _periphExecFormFingerprint() {
            const form = document.getElementById('periph-exec-form');
            return form ? JSON.stringify(Array.from(form.querySelectorAll('input,select,textarea'), field =>
                [field.id, field.type, field.value, field.checked, field.disabled])) : null;
        },

        _periphExecContractNotes(data, rule) {
            const actions = rule.actions || [];
            const contracts = data.actionContracts;
            const complete = data.actionContractsVersion === 1 && data.actionContractsTruncated === false &&
                Array.isArray(contracts) && contracts.length === actions.length && contracts.every((item, i) =>
                    item && item.index === i && item.actionType === actions[i].actionType);
            if (!complete) return {review: false, notes: ['动作执行说明尚未完整提供；结果、超时和重试边界未知。']};
            let review = false;
            const notes = contracts.map((item, i) => {
                const results = {sdk_ack_only: '成功仅表示驱动接受写入，未验证物理输出。',
                    local_acceptance_only: '成功仅表示本机接受命令，未验证物理输出。',
                    sample_with_quality: '结果是带质量与时效的采样，可能复用保留原采样时间的缓存；失败或过期不能当作有效读数。',
                    recorder_status_only: '手动执行只读取SD记录状态；规则启用后才持续记录，不启动或恢复记录器，不重放失败写入。',
                    controller_status_only: '此动作只读取温控状态；实际输出由持续运行的温控器管理。'};
                let text = item.coverage === 'described' ? results[item.result] : '';
                if (!text) return '动作 ' + (i + 1) + '：结果、超时和重试边界未知。';
                if (item.idempotence === 'non_idempotent') {
                    review = true;
                    text += '翻转动作重复执行会再次改变输出，结果不明时不要直接重试。';
                }
                if (item.timeoutScope === 'conversion_budget_not_whole_action_deadline' &&
                    Number.isInteger(item.timeoutBudgetMs) && item.timeoutBudgetMs > 0) {
                    text += '转换等待预算 ' + item.timeoutBudgetMs + ' ms，不是整个动作的超时保证。';
                } else text += '整个动作的完成时限未知。';
                if (item.replay === 'release_cleanup_only_after_stop') text += '停止失败后只重试资源清理，不重放输出。';
                else if (item.replay === 'no_automatic_replay') text += '系统不会自动重放此动作。';
                else text += '重试策略未知。';
                return '动作 ' + (i + 1) + '：' + text;
            });
            return {review, notes};
        },

        async _preflightPeriphExecSave(rule, isEdit, seq, generation, active) {
            const response = await apiPostJson('/api/periph-exec/preflight', {operation: isEdit ? 'update' : 'add', rule});
            if (!active()) return false;
            const data = response?.data;
            if (response?.success !== true || !data || typeof data.allowed !== 'boolean' ||
                typeof data.requiresConfirmation !== 'boolean' || !Array.isArray(data.issues)) {
                this._pePreflightReview = null;
                throw new Error('保存前检查未返回有效结果');
            }
            const contract = this._periphExecContractNotes(data, rule);
            const allowed = data.allowed && !data.issues.some(issue => issue?.severity === 'error');
            const needsReview = allowed && (data.requiresConfirmation || contract.review);
            const fingerprint = JSON.stringify([rule, data]);
            const previous = this._pePreflightReview;
            const confirmed = needsReview && previous?.seq === seq && previous?.generation === generation &&
                previous?.fingerprint === fingerprint;
            this._pePreflightReview = needsReview ? {seq, generation, fingerprint} : null;
            let host = document.getElementById('periph-exec-preflight');
            if (!host) {
                host = document.createElement('div');
                host.id = 'periph-exec-preflight';
                document.getElementById('periph-exec-error')?.insertAdjacentElement('beforebegin', host);
            }
            host.className = 'u-hint-bar ' + (allowed ? 'u-hint-bar-info' : 'u-hint-bar-warning');
            host.textContent = '';
            const title = document.createElement('strong');
            title.textContent = !allowed ? '保存前检查未通过' : needsReview && !confirmed ? '请阅读检查结果，再次点击保存确认' : '保存前检查通过';
            host.appendChild(title);
            for (const issue of data.issues) {
                const line = document.createElement('div');
                line.textContent = String(issue?.message || '检查结果未知');
                host.appendChild(line);
            }
            if (!allowed || needsReview) {
                const details = document.createElement('details');
                const summary = document.createElement('summary');
                summary.textContent = '动作结果与重试说明';
                details.appendChild(summary);
                for (const note of contract.notes) {
                    const line = document.createElement('div');
                    line.textContent = note;
                    details.appendChild(line);
                }
                host.appendChild(details);
            }
            return allowed && (!needsReview || confirmed);
        },

        _renderPeriphExecLazyActionPanel(block, actionType, data, forceMount) {
            if (!block) return;
            var host = block.querySelector('.pe-action-lazy-host');
            if (!host) return;
            var oldType = block.dataset.peLazyActionType;
            var requestedType = String(actionType);
            // Loading may overlap a trigger rebuild or an action-type switch.
            // Retain saved input until the actual controls have been mounted.
            if (actionType === 39 || actionType === 40) {
                const key = actionType === 39 ? '_peThermostatData' : '_peSdRecorderData';
                if (data) block[key] = data;
                else data = block[key];
            }
            var targetId = block.querySelector('.pe-target-periph')?.value || (data && data.targetPeriphId) || '';
            if (!forceMount && !data && oldType === requestedType) {
                if (actionType !== 10) return;
                var hasNeoPixelPanel = !!host.querySelector('.pe-neopixel-preset');
                if (hasNeoPixelPanel === this._isNeoPixelTarget(targetId)) return;
            }
            block._peLazyPanelDrafts = block._peLazyPanelDrafts || {};
            if (oldType !== undefined && host.querySelector('input, select, textarea')) {
                block._peLazyPanelDrafts[oldType] = this._snapshotPeriphExecLazyPanel(host);
            }
            block._peLazyLoadToken = (block._peLazyLoadToken || 0) + 1;
            host.innerHTML = '';
            var actionValue = data && data.actionValue ? data.actionValue : '';
            var moduleName = this._getPeriphExecLazyActionModule(actionType, targetId);
            var moduleReady = !moduleName ||
                (moduleName === 'periph-exec-sensor' && typeof this[actionType === 40 ? '_renderSdRecorderPanel' : actionType === 39 ? '_renderThermostatPanel' : '_buildPeriphExecSensorPanel'] === 'function') ||
                (moduleName === 'periph-exec-effects' && typeof this._buildPeriphExecEffectsDisplayPanel === 'function');
            block.dataset.peLazyActionType = requestedType;
            if (!moduleReady) {
                if (forceMount) {
                    block.dataset.peLazyModuleError = moduleName;
                    host.innerHTML = '<small class="pe-help-text">配置模块加载失败，请刷新页面重试</small>';
                    return;
                }
                this._loadPeriphExecLazyActionModule(moduleName, block, actionType, data);
                return;
            }
            delete block.dataset.peLazyModuleError;
            if (actionType === 18) {
                host.innerHTML = this._buildPeriphExecModbusPanel();
                this._populateModbusDevicePanel(host.querySelector('.pe-poll-tasks-list'), actionValue);
            } else if (actionType === 19) {
                host.innerHTML = this._buildPeriphExecSensorPanel(data || {});
            } else if (actionType === 39) {
                host.innerHTML = '<div class="pe-thermostat-group">' + this._renderThermostatPanel(actionValue) + '</div>';
            } else if (actionType === 40) {
                host.innerHTML = '<div class="pe-sd-record-group">' + this._renderSdRecorderPanel(actionValue) + '</div>';
            } else if (actionType === 15 || actionType === 21) {
                host.innerHTML = this._buildPeriphExecInlineActionPanel(actionType, actionValue);
            } else if (moduleName === 'periph-exec-effects') {
                host.innerHTML = this._buildPeriphExecEffectsDisplayPanel(actionType, actionValue);
            }
            this._restorePeriphExecLazyPanel(host, block._peLazyPanelDrafts[requestedType]);
            if (actionType === 19) {
                var category = host.querySelector('.pe-sensor-category');
                this._populateSensorPeriphSelect(block, category ? category.value : 'analog', targetId);
                if (category) this.onSensorCategoryChange(category, this._getPeriphExecBlockIndex(block), true);
                if (category?.value === 'configured') {
                    var configuredSensor = {};
                    try { configuredSensor = JSON.parse(actionValue || '{}'); } catch (error) {}
                    this._syncConfiguredSensorAction(block, configuredSensor);
                }
            }
            if (actionType === 21 && actionValue) {
                var eventSelect = host.querySelector('.pe-trigger-event-select');
                if (eventSelect) eventSelect.value = actionValue;
            }
            if (actionType === 27) {
                var oled = host.querySelector('.pe-action-value-oled');
                var counter = host.querySelector('.pe-oled-counter');
                if (oled && counter) {
                    oled.addEventListener('input', function() {
                        var lines = this.value ? this.value.split('\n').length : 0;
                        counter.textContent = '已输入 ' + this.value.length + '/512 字符，' + lines + '/6 行';
                        counter.style.color = lines > 6 ? '#e74c3c' : '';
                    });
                    oled.dispatchEvent(new Event('input'));
                }
            }
            if (moduleName === 'periph-exec-effects') {
                this._initNeoPixelPresetFromData(block, actionValue || block.querySelector('.pe-action-value')?.value || '', actionType);
                this._updateNeoPixelPresetVisibility(block);
            }
        },

        _uartSendHelp() {
            return '发送文本保留首尾空格；$value 替换后最多256字节，不自动换行。JSON请用 {"action":"send","value":"文本"}，目标必须与所选外设一致。本地接受不代表对端收到；失败可能已部分发送，勿盲目重试。';
        },

        _thermostatTargets() {
            return (this._pePeripherals || []).filter(p => Number(p.type) === 12 && p.enabled === true &&
                Array.isArray(p.pins) && p.pins.length === 1 && Number.isInteger(Number(p.pins[0])) &&
                Number(p.pins[0]) >= 0 && Number(p.pins[0]) < 255 &&
                p.params && !['expander', 1, '1'].includes(p.params.ioSource) &&
                !p.params.passive && !p.params.invertLogic && Number(p.params.initialState) === 1);
        },


        _populateThermostatTargetSelect(select, selected) {
            if (!select) return;
            const targets = this._thermostatTargets();
            select.innerHTML = '<option value="">-- 选择专用低电平吸合输出 --</option>' +
                targets.map(p => '<option value="' + escapeHtml(p.id) + '">' + escapeHtml(p.name || p.id) + '</option>').join('') +
                (selected && !targets.some(p => p.id === selected) ?
                    '<option value="' + escapeHtml(selected) + '" disabled>原输出不符合温控安全条件</option>' : '');
            select.value = selected || '';
        },

        _resetPeriphExecTemplateWizard(isNew) {
            const root = field('wizard');
            if (!root) return;
            root.classList.toggle('is-hidden', !isNew);
            root.open = false;
            this._peTemplatePreview = null;
            if (!isNew) return;
            if (!root.querySelector('#pe-template-kind')) root.innerHTML = wizardMarkup;
            if (!field('kind').querySelector('option[value="sd-record"]')) {
                const option = document.createElement('option');
                option.value = 'sd-record'; option.textContent = '数值采集来源 → SD 持续记录'; field('kind').appendChild(option);
                root.querySelector('summary').textContent = '从采集或记录模板开始（新增规则）';
            }
            field('kind').value = ''; field('interval').value = '60'; field('property').value = '';
            field('modbus-reviewed').checked = false;
            field('confirm').checked = false; field('apply').disabled = true;
            field('preview').textContent = '先选择模板与实际外设，再预览。只生成禁用草稿，不会保存或执行。';
            this._refreshPeriphExecTemplateChoices(true);
            if (root.dataset.bound) return;
            root.dataset.bound = '1';
            root.addEventListener('input', event => {
                if (event.target === field('confirm')) return;
                this._invalidatePeriphExecTemplatePreview();
            });
            root.addEventListener('change', event => {
                if (event.target === field('confirm')) {
                    field('apply').disabled = !(event.target.checked && this._peTemplatePreview);
                    return;
                }
                if (event.target === field('kind')) this._refreshPeriphExecTemplateChoices(true);
                if (event.target === field('target')) this._refreshPeriphExecTemplateChoices(false);
                if (event.target === field('kind') || event.target === field('target')) field('modbus-reviewed').checked = false;
                this._invalidatePeriphExecTemplatePreview();
            });
            field('review').addEventListener('click', () => this.previewPeriphExecTemplate());
            field('apply').addEventListener('click', () => this.applyPeriphExecTemplate());
        },

        _invalidatePeriphExecTemplatePreview() {
            this._peTemplatePreview = null;
            field('confirm').checked = false; field('apply').disabled = true;
            field('preview').textContent = '选择已变化，请重新预览；当前规则表单未改变。';
        },

        _getPeriphExecTemplateTarget(kind, id) {
            const type = kind === 'rtc-read' ? 50 : kind === 'configured-sensor' ? 38 : ['sd-status', 'sd-record'].includes(kind) ? 37 : -1;
            const peripheral = (this._pePeripherals || []).find(p => String(p.id) === id && Number(p.type) === type);
            const descriptor = (this._peTypeCatalog || []).find(d => Number(d.value ?? d.type) === type);
            const params = peripheral?.params || {};
            const driverName = params.driverName || params.driver || peripheral?.driver || '';
            const driver = (descriptor?.driverDescriptors || []).find(d => String(d.name).toLowerCase() === String(driverName).toLowerCase());
            const channels = (driver?.channels || []).filter(c => (!c.mode || Number(c.mode) === (Number(params.mode || 0) === 1 ? 2 : 1)) &&
                (!c.singleMuxOnly || !Number(params.scanMask)) &&
                (c.scanMux === undefined || !!(Number(params.scanMask) & (1 << Number(c.scanMux)))));
            return { peripheral, descriptor, driver, channels };
        },

        _refreshPeriphExecTemplateChoices(resetTargets) {
            if (!field('wizard') || !field('kind')) return;
            const kind = field('kind').value;
            const sensor = kind === 'configured-sensor';
            const modbus = kind === 'configured-modbus';
            const recorder = kind === 'sd-record';
            const type = sensor ? 38 : kind === 'rtc-read' ? 50 : (kind === 'sd-status' || recorder) ? 37 : -1;
            if (resetTargets) {
                const peripherals = this._peEditorDataReady === true ? (this._pePeripherals || []) : [];
                const targets = modbus && this._peEditorDataReady === true ? (this._masterTasks || []).map((task, index) => ({
                    id: String(index), label: (task.name || task.label || '任务 ' + index) + ' · FC' + task.functionCode +
                        ' @' + task.startAddress + ' ×' + task.quantity + (task.enabled === false ? ' · 未启用' : '')
                })) : peripherals.filter(p => Number(p.type) === type).map(p => ({
                    id: String(p.id), label: (p.name || p.id) + (p.enabled === false ? ' · 未启用' : '')
                }));
                choices(field('target'), targets, '-- 显式选择外设或任务 --');
            }
            const details = this._getPeriphExecTemplateTarget(kind, field('target').value);
            choices(field('channel'), recorder ? (this._peSensorSources || []).map(s => ({id: s.id, label: s.label || s.id})) : details.channels.map(c => ({id: c.name, label: (c.label || c.name) + (c.unit ? ' (' + c.unit + ')' : '')})), '-- 显式选择通道 --');
            field('channel-group').classList.toggle('is-hidden', !sensor && !recorder);
            field('channel-group').querySelector('label').textContent = recorder ? '已保存的数值采集来源' : '驱动测量通道';
            const interval = field('interval');
            if (recorder) interval.value = '1';
            else if (interval.disabled) interval.value = '60';
            interval.disabled = recorder; interval.min = recorder ? '1' : '5';
            field('property-group').classList.toggle('is-hidden', !sensor);
            field('modbus-group').classList.toggle('is-hidden', !modbus);
            field('review').disabled = this._peEditorDataReady !== true;
        },

        _buildPeriphExecTemplateDraft(input) {
            if (this._peEditorDataReady !== true) throw new Error('配置数据尚未加载成功');
            if (document.getElementById('periph-exec-original-id')?.value) throw new Error('模板只用于新增规则，不替换已保存规则');
            if (!['rtc-read', 'configured-sensor', 'configured-modbus', 'sd-status', 'sd-record'].includes(input.kind)) throw new Error('请选择采集或记录模板');
            if (input.kind === 'configured-modbus') {
                const config = this._peTemplateModbusConfig;
                if (!config || config.enabled !== true || config.degraded || config.transferType !== 0) throw new Error('需要已启用、非降级的 Modbus JSON 采集配置');
                if (!/^(0|[1-9][0-9]*)$/.test(input.target)) throw new Error('请显式选择已保存的采集任务');
                const index = Number(input.target), task = (this._masterTasks || [])[index];
                if (!task || task !== config.master?.tasks?.[index] || task.enabled !== true) throw new Error('任务缺失、未启用或配置已变化');
                if (![3,4].includes(task.functionCode) || !Number.isInteger(task.slaveAddress) || task.slaveAddress < 1 || task.slaveAddress > 247 ||
                    !Number.isInteger(task.startAddress) || task.startAddress < 0 || !Number.isInteger(task.quantity) || task.quantity < 1 ||
                    task.quantity > 64 || task.startAddress + task.quantity > 65536) throw new Error('仅支持有效地址范围的 FC03/FC04、1～64 个寄存器只读任务');
                if (!Array.isArray(task.mappings) || !task.mappings.length || task.mappings.some(m => !m ||
                    typeof m.sensorId !== 'string' || !/^[A-Za-z_][A-Za-z0-9_]{0,62}$/.test(m.sensorId) || !Number.isInteger(m.regOffset) || m.regOffset < 0 || m.regOffset >= task.quantity)) throw new Error('请先在协议配置核对有效字段映射');
                if (input.reviewed !== true) throw new Error('请先核对仪表型号、地址、字序、倍率和单位并确认');
                const interval = Number(input.interval);
                if (!Number.isInteger(interval) || interval < 5 || interval > 86400) throw new Error('采集间隔须为 5～86400 秒的整数');
                return this._makePeriphExecCollectionDraft(task.name || task.label || 'Modbus', interval, {
                    actionType: 18, targetPeriphId: '', actionValue: JSON.stringify({poll:[index]}), useReceivedValue: false
                });
            }
            const details = this._getPeriphExecTemplateTarget(input.kind, input.target);
            const p = details.peripheral, d = details.descriptor;
            if (!p || !input.target || utf8Size(input.target) > 40) throw new Error('请选择仍存在且类型匹配的外设');
            if (!d || d.implemented === false || d.compiled === false) throw new Error('当前固件没有声明该外设能力');
            if (p.enabled === false || p.featureActive === false || d.featureActive === false) throw new Error('外设或所需功能未启用，请先完成外设配置');
            if (input.kind === 'sd-record') {
                const schema = this._sdRecorderConfigSchema();
                if (!schema || p.enabled !== true) throw new Error('需要完整 SD 记录参数及已启用的 SD 外设');
                if (!(this._peSensorSources || []).some(s => s.id === input.channel)) throw new Error('请选择已保存的数值采集来源');
                const cfg = {sourceRef: input.channel};
                for (const key of ['sampleSec', 'maxAgeSec', 'flushSec', 'fileKiB', 'fileCount']) cfg[key] = schema[key].defaultValue;
                return {name: 'SD 数值记录', enabled: false, reportAfterExec: false, recordHistory: false, allowPanelControl: false,
                    triggers: [{triggerType: 1, timerMode: 0, intervalSec: 1}], actions: [{actionType: 40, targetPeriphId: input.target,
                        actionValue: JSON.stringify(cfg), useReceivedValue: false, syncDelayMs: 0}]};
            }
            const interval = Number(input.interval);
            const minSeconds = Math.max(5, Math.ceil(Number(details.driver?.minIntervalMs || 0) / 1000));
            if (!Number.isInteger(interval) || interval < minSeconds || interval > 86400) throw new Error('采集间隔须为 ' + minSeconds + '～86400 秒的整数');
            let action;
            if (input.kind === 'rtc-read') {
                action = {actionType: 36, targetPeriphId: input.target, actionValue: '', useReceivedValue: false};
            } else if (input.kind === 'sd-status') {
                action = {actionType: 38, targetPeriphId: input.target, actionValue: '', useReceivedValue: false};
            } else {
                const channel = details.channels.find(c => c.name === input.channel);
                if (!details.driver || details.driver.compiled === false || details.driver.featureActive === false || !channel) throw new Error('请选择当前固件和工作模式实际支持的通道');
                if (!/^[A-Za-z_][A-Za-z0-9_]{0,62}$/.test(input.property)) throw new Error('物模型字段须为 1～63 个字母、数字或下划线，不能以数字开头');
                action = {actionType: 19, targetPeriphId: input.target, useReceivedValue: false, actionValue: JSON.stringify({
                    periphId: input.target, sensorCategory: details.driver.name, channelField: channel.name, dataField: input.property,
                    scaleFactor: 1, offset: 0, decimalPlaces: 2, sensorLabel: channel.label || channel.name, unit: channel.unit || ''
                })};
            }
            return this._makePeriphExecCollectionDraft(p.name || p.id, interval, action);
        },

        _makePeriphExecCollectionDraft(label, interval, action) {
            return {name: draftName(label), enabled: false,
                reportAfterExec: true, recordHistory: false, allowPanelControl: false,
                triggers: [{triggerType: 1, timerMode: 0, intervalSec: interval}], actions: [action]};
        },

        _readPeriphExecTemplateInput() {
            return {kind: field('kind').value, target: field('target').value, channel: field('channel').value,
                property: field('property').value.trim(), interval: field('interval').value, reviewed: field('modbus-reviewed').checked};
        },

        _periphExecTemplateIdentity(input, draft) {
            return JSON.stringify({input, draft, protocol: input.kind === 'configured-modbus' ? this._peTemplateModbusConfig : null});
        },

        previewPeriphExecTemplate() {
            this._invalidatePeriphExecTemplatePreview();
            try {
                const input = this._readPeriphExecTemplateInput();
                const draft = this._buildPeriphExecTemplateDraft(input);
                this._peTemplatePreview = this._periphExecTemplateIdentity(input, draft);
                if (input.kind === 'sd-record') {
                    field('preview').textContent = 'SD 记录草稿：' + input.channel + ' → ' + input.target + '。保持禁用；保存启用后才持续写卡，手动执行只读状态。来源规则须唯一、启用并记录数值历史；先检查采样质量、挂载和轮转上限，旧文件会被覆盖。';
                    return;
                }
                field('preview').textContent = draft.name + '：每 ' + draft.triggers[0].intervalSec + ' 秒读取 ' + input.target +
                    (input.kind === 'sd-status' ? ' 的状态快照（以外设 ID 绑定 JSON 文本属性，含容量质量与年龄，不读写卡）' : input.kind === 'rtc-read' ? ' 的未带时区日历（不证明走时准确，不写时钟）' : input.kind === 'configured-modbus' ?
                        ' 号已保存只读任务（沿用原字段映射；未自动验证仪表/串口/倍率，重排任务后需复查）' : ' / ' + input.channel + ' → ' + input.property) +
                    '。规则保持禁用；执行后请求上报，MQTT 连接、平台字段与实际收到数据需另行验证。无数值历史、无屏幕控制，不改变接线、校准或协议配置。应用仅替换当前未保存表单，之后仍需检查并手动保存、启用。';
            } catch (error) { field('preview').textContent = error.message; }
        },

        applyPeriphExecTemplate() {
            if (!this.guardDeveloperModeAction() || this._peSavePending || !field('confirm').checked || !this._peTemplatePreview) return;
            let draft;
            try {
                const input = this._readPeriphExecTemplateInput();
                draft = this._buildPeriphExecTemplateDraft(input);
                if (this._periphExecTemplateIdentity(input, draft) !== this._peTemplatePreview) throw new Error('输入或能力已变化，请重新预览');
            } catch (error) {
                this._invalidatePeriphExecTemplatePreview(); field('preview').textContent = error.message; return;
            }
            // Only the new, unsaved editor changes; the normal save/preflight path remains authoritative.
            document.getElementById('periph-exec-name').value = draft.name;
            document.getElementById('periph-exec-enabled').checked = false;
            document.getElementById('periph-exec-report').checked = draft.reportAfterExec;
            document.getElementById('periph-exec-record-history').checked = false;
            document.getElementById('periph-exec-panel-control').checked = false;
            document.getElementById('periph-exec-triggers').replaceChildren();
            document.getElementById('periph-exec-actions').replaceChildren();
            this._createPeriphExecTriggerElement(draft.triggers[0], 0);
            this._createPeriphExecActionElement(draft.actions[0], 0);
            this._invalidatePeriphExecTemplatePreview();
            field('preview').textContent = '禁用草稿已填入表单，尚未保存或执行。检查下方字段，保存后核对接线、数据质量和上报结果，再决定是否启用。';
        }
    });
})();
