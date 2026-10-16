/**
 * Configuration transfer and board migration, loaded on the first user action.
 * Keep schemas, acknowledgement checks and recovery paths out of the device
 * landing module without changing their wire contracts.
 */
(function() {
    const MAX_CONFIG_TRANSFER_BYTES = 128 * 1024;

    const MAX_CONFIG_BUNDLE_BYTES = 512 * 1024;
    const CONFIG_MIGRATION_FILES = ['device.json', 'features.json', 'peripherals.json', 'periph_exec.json',
        'rule_scripts.json', 'screen_panel.json', 'point_history.json', 'ir_keys.json'];
    const CONFIG_PLAN_FILES = ['features.json', 'peripherals.json', 'periph_exec.json', 'screen_panel.json', 'point_history.json'];
    const CONFIG_TRANSFER_LABELS = {
        'all': '全部配置包',
        'device.json': '设备信息',
        'features.json': '功能配置',
        'network.json': '网络配置',
        'peripherals.json': '外设配置',
        'periph_exec.json': '外设执行',
        'protocol.json': '通信协议',
        'users.json': '用户配置',
        'auth.json': '安全认证',
        'rule_scripts.json': '规则脚本',
        'screen_panel.json': '屏幕布局',
        'point_history.json': '点位历史配置'
    };


    // Do not fetch, parse or retain these templates with unrelated global
    // modals. The existing mount/unmount lifecycle still owns their DOM.
    if (typeof PageLoader !== 'undefined') {
        PageLoader._cacheModalTemplates("<div id=\"config-board-migration-modal\" class=\"modal\"><div class=\"modal-content u-modal-content-560\"><div class=\"modal-header\"><h2 class=\"modal-title\">换板接线预览</h2><button class=\"modal-close-btn\" type=\"button\">&times;</button></div><div class=\"modal-body\" style=\"padding:16px 20px\"><p id=\"config-board-profile\" class=\"u-help\"></p><div id=\"config-board-pins\" class=\"config-form-grid\"></div><p id=\"config-board-status\" role=\"status\" aria-live=\"polite\"></p><label class=\"fb-checkbox\"><input id=\"config-board-ack\" type=\"checkbox\"><span>已核对源与目标接线差异，并理解所有外设与规则将停用；旧配置已备份</span></label></div><div class=\"modal-footer\"><button id=\"config-board-cancel\" class=\"fb-btn\" type=\"button\">取消</button><button id=\"config-board-confirm\" class=\"fb-btn fb-btn-warning\" type=\"button\" disabled>暂存并预检</button></div></div></div><div id=\"config-transfer-modal\" class=\"modal\"><div class=\"modal-content u-modal-content-560\"><div class=\"modal-header\"><h2 class=\"modal-title\" id=\"config-transfer-modal-title\">选择配置文件</h2><button class=\"modal-close-btn\">&times;</button></div><div class=\"modal-body\" style=\"padding:16px 20px\"><div style=\"display:flex;align-items:center;justify-content:space-between;margin-bottom:12px\"><label class=\"fb-checkbox\" style=\"font-weight:600\"><input type=\"checkbox\" id=\"config-transfer-select-all\" checked><span>全选</span></label><span id=\"config-transfer-summary\" style=\"font-size:12px;color:var(--text-secondary)\"></span></div><div id=\"config-transfer-list\" style=\"display:flex;flex-direction:column;gap:8px\"><!-- 动态填充配置文件复选框 --></div><div id=\"config-transfer-preview\" class=\"u-note-card u-note-card-warning is-hidden\" role=\"status\" aria-live=\"polite\"></div><label id=\"config-transfer-ack-row\" class=\"fb-checkbox is-hidden\"><input id=\"config-transfer-ack\" type=\"checkbox\"><span>已审查所选配置风险，已保全当前配置并确认恢复路径；预览不代表兼容通过</span></label></div><div class=\"modal-footer\"><button class=\"fb-btn\" id=\"config-transfer-cancel-btn\" type=\"button\">取消</button><button class=\"fb-btn fb-btn-warning\" id=\"config-transfer-confirm-btn\" type=\"button\">确认</button></div></div></div>", '',
            ['config-transfer-modal', 'config-board-migration-modal']);
    }

    AppState.registerModule('device-config-transfer', {
        _configTransferReady: true,

        _configTransferFetch(url) {
            // Current firmware authenticates browser requests with the
            // same-origin HttpOnly session cookie.  Do not attach a legacy
            // bearer token from localStorage: the server gives an explicit
            // Authorization header precedence, so a stale token would mask a
            // valid cookie and turn config transfer into a misleading 401.
            return fetch(url, {
                method: 'GET',
                credentials: 'include',
                cache: 'no-store'
            });
        },

        _normalizeConfigFileName(name) {
            return String(name || '')
                .replace(/^fastbee-config-/, '')
                .replace(/\s+\(\d+\)(?=\.json$)/i, '');
        },

        /**
         * 根据 JSON 内容自动检测配置文件类型，返回规范文件名
         * 当文件名不是已知配置名时，用于 AI 生成配置等场景的自动识别
         */
        _detectConfigFileName(name, parsed) {
            // 已经是已知配置文件名，无需检测
            if (CONFIG_TRANSFER_LABELS[name]) return name;
            if (!parsed || typeof parsed !== 'object') return name;

            // 按内容特征匹配
            if (parsed.storageVersion === 1 && parsed.config?.schemaVersion === 1 && Array.isArray(parsed.config.pages)) return 'screen_panel.json';
            if (parsed.schemaVersion === 1 && typeof parsed.enabled === 'boolean' && Array.isArray(parsed.sourceAllowlist)) return 'point_history.json';
            if (Array.isArray(parsed.peripherals)) return 'peripherals.json';
            if (Array.isArray(parsed.rules)) return 'periph_exec.json';
            if (parsed.staSSID !== undefined || parsed.apSSID !== undefined || parsed.mode !== undefined) return 'network.json';
            if (parsed.deviceName !== undefined || parsed.chipType !== undefined) return 'device.json';
            if (parsed.mqtt !== undefined || parsed.server !== undefined) return 'protocol.json';
            // 功能配置：功能页导出的包装格式（fastbee-feature-config）或设备原始
            // features.json；也兼容 feature-settings.json/functions.json 等友好命名
            if (parsed.type === 'fastbee-feature-config'
                || (parsed.features && typeof parsed.features === 'object' && !Array.isArray(parsed.features))) {
                return 'features.json';
            }
            return name;
        },

        _getConfigTransferLabel(name) {
            return CONFIG_TRANSFER_LABELS[name] || name || '--';
        },

        // Deliberately descriptive, not a board/schema validator. Never renders imported values or secrets.
        _buildConfigTransferPreview(item) {
            const entry = this._validateConfigTransferEntry(item);
            const config = JSON.parse(entry.content);
            const lines = [], warnings = [];
            const object = config && typeof config === 'object' && !Array.isArray(config);
            if (!object) return {lines: ['配置根结构不是对象'], warnings: ['结构未验证，服务端可能拒绝；预览不代表兼容']};
            if (entry.name === 'peripherals.json') {
                const list = Array.isArray(config.peripherals) ? config.peripherals : [];
                const ids = list.filter(p => p && typeof p.id === 'string').map(p => p.id);
                lines.push('外设条目：' + list.length + '；显式启用：' + list.filter(p => p?.enabled === true || p?.enabled === 1).length);
                if (!Array.isArray(config.peripherals)) warnings.push('缺少 peripherals 数组，待服务端验证');
                if (new Set(ids).size !== ids.length || ids.length !== list.length || ids.some(id => !id)) warnings.push('外设 ID 重复或缺失，依赖可能失效');
                warnings.push('整份参与资源预检，下次重启应用；目标配置变化会阻止迁移');
                warnings.push('MCP23017须禁用导入，重启后复核整组再启用；Modbus外设由协议管理');
                warnings.push('GPIO、总线、型号、供电、初始化及危险输出未验证；换板须用“换板接线迁移”逐项选目标GPIO');
            } else if (entry.name === 'periph_exec.json') {
                const rules = Array.isArray(config.rules) ? config.rules : [];
                const actions = rules.flatMap(r => Array.isArray(r?.actions) ? r.actions : []);
                const enabled = rules.filter(r => r?.enabled === true || r?.enabled === 1).length;
                lines.push('执行规则：' + rules.length + '；显式启用：' + enabled + '；动作条目：' + actions.length);
                if (!Array.isArray(config.rules)) warnings.push('缺少 rules 数组，旧格式待服务端验证');
                if (enabled) warnings.push('已启用规则在应用或重启后可能自动执行，请先核对现场安全');
                if (rules.some(r => ![true,false,0,1].includes(r?.enabled))) warnings.push('启用状态未明确，不推断为禁用，默认行为待服务端核验');
                if (actions.some(a => [35,36,37].includes(Number(a?.actionType)))) warnings.push('RTC动作35/36/37可能被旧固件拒绝，禁用规则不解决版本不兼容');
                if (actions.some(a => Number(a?.actionType) === 38)) warnings.push('SD动作38需固件支持；上报带质量的文本快照，非实时容量');
                if (actions.some(a => Number(a?.actionType) === 39)) warnings.push('温控动作39需固件支持及有效摄氏来源，仅接受低电平吸合、上电高电平的独立原生数字输出');
                if (actions.some(a => Number(a?.actionType) === 40)) warnings.push('SD记录动作40须禁用导入；目标为已启用的SD，来源须唯一、启用且记录数值历史。启用后持续写卡，手动执行只读状态；导入不包含日志或恢复故障。');
                if (actions.some(a => [10,29].includes(Number(a?.actionType)))) warnings.push('通用/串口发送：UART替换后限256字节，JSON目标须匹配所选外设；旧参数须预检，失败可能已部分发送，勿自动重试');
                warnings.push('目标设备尚未解析外设/触发源/规则引用及动作能力；ID不自动修复');
            } else if (entry.name === 'rule_scripts.json') {
                const rules = Array.isArray(config.rules) ? config.rules : [];
                lines.push('转换脚本：' + rules.length + '；显式启用：' + rules.filter(rule => rule?.enabled === true).length);
                warnings.push('整份替换，限64条/24KB；随整组下次启动应用，暂存不改变运行中的转换');
                if (!Array.isArray(config.rules) || rules.some(rule => typeof rule?.enabled !== 'boolean')) warnings.push('须提供rules数组及逐条布尔启用状态，由服务端验证');
                if (rules.some(rule => rule?.enabled === true)) warnings.push('启用脚本会改变接收/上报内容，请核对转换和重定向目标');
            } else if (entry.name === 'screen_panel.json') {
                const pages = Array.isArray(config.config?.pages) ? config.config.pages : [];
                lines.push('屏幕页面：' + pages.length + '；卡片条目：' + pages.reduce((sum, page) => sum + (Array.isArray(page?.cards) ? page.cards.length : 0), 0));
                warnings.push('整份替换布局；目标设备须有可用显示、数据源和控制规则，由服务端校验绑定');
                warnings.push('随整组摘要确认，下次启动应用后核验屏幕');
                if (config.touchUiMigration !== 2) warnings.push('缺少触摸布局迁移标记，请在屏幕编辑器检查后重新导出');
            } else if (entry.name === 'point_history.json') {
                lines.push('历史采集：' + (config.enabled === true ? '启用' : config.enabled === false ? '禁用' : '待验证') + '；来源白名单：' + (Array.isArray(config.sourceAllowlist) ? config.sourceAllowlist.length : '待验证'));
                warnings.push('仅迁移配置，保留运行历史缓存，不导入样本；空白名单允许任意来源');
                warnings.push('来源ID原样保留，须在目标核验存在性和采样质量');
            } else if (entry.name === 'features.json') {
                const settings = config.features;
                lines.push('功能开关：' + (settings && typeof settings === 'object' && !Array.isArray(settings) ? Object.keys(settings).length : '结构待验证'));
                warnings.push('固件裁剪、依赖、预算及重启后实例未验证');
            } else if (entry.name === 'device.json') {
                lines.push('仅合并名称、位置、描述、时间和日志设置');
                warnings.push('身份、产品/账号绑定、开发模式等字段保留目标值；来源身份和凭据不展示');
            } else if (['network.json','protocol.json','users.json','auth.json'].includes(entry.name)) {
                lines.push('敏感配置仅显示类别，隐藏身份、地址、口令、令牌和配置值');
                warnings.push('整组迁移不支持，请取消选择；保留目标网络、协议、用户和凭据。普通备份可能含秘密，须妥善保管');
            } else {
                lines.push('其它配置：内容未展示');
                warnings.push('结构、脚本和能力未检查，以服务端结果为准');
            }
            warnings.push('仅为本地摘要，非兼容证明；目标固件将校验完整候选，不自动修复');
            return {lines, warnings};
        },

        // Mirror the pure PeripheralActionContract target predicate only.
        // null means insufficient/noncanonical metadata, not compatibility.
        // Runtime availability, command values and board wiring are not inferred.
        _matchesConfigTransferActionTarget(action, target) {
            const type = target?.type, p = target?.params || {};
            if (!Number.isInteger(type)) return null;
            if (type >= 11 && type <= 25 && [3,4,5].includes(action)) {
                if (p.ioSource !== undefined && p.ioSource !== 0 && p.ioSource !== 1) return null;
                if (p.ioSource === 1) return false;
            }
            switch (action) {
                case 0: case 1: case 13: case 14: case 30: return [12,16,17].includes(type);
                case 2: return [12,17].includes(type);
                case 3: return [16,17].includes(type);
                case 4:
                    if ([16,17,41].includes(type)) return true;
                    if (type !== 12) return false;
                    if (p.passive !== undefined && ![true,false,0,1].includes(p.passive)) return null;
                    return p.passive === true || p.passive === 1;
                case 5: return type === 27;
                case 10: return true; // Command-dependent checks remain on device.
                case 11: return type === 45;
                case 12: return type === 42;
                case 16: case 17: return type === 51;
                case 19: return [11,13,14,18,19,20,15,26,38,44,43,49,48].includes(type);
                case 24: case 25: case 26: return type === 47;
                case 27: return type === 36;
                case 28: return type === 48;
                case 29: return type === 1;
                case 35: case 36: case 37: return type === 50;
                case 38: case 40: return type === 37;
                case 39:
                    if (type !== 12 || p.ioSource === 1 || p.passive === true || p.invertLogic === true) return false;
                    if (!Array.isArray(target.pins) || p.initialState === undefined || p.ioSource === undefined ||
                        typeof p.passive !== 'boolean' || typeof p.invertLogic !== 'boolean') return null;
                    return target.pins.length === 1 && p.ioSource === 0 && p.initialState === 1;
                default: return null;
            }
        },

        // Inspect selected documents. Only bounded manifest identifiers may be
        // displayed; configuration values/secrets never enter this summary.
        _buildConfigTransferDependencyPreview(files) {
            const lines = [], warnings = [], errors = [], documents = new Map();
            const manifests = new Set();
            for (const file of files) {
                if (file.planRequirements && !manifests.has(file.planRequirements)) {
                    const req = file.planRequirements;
                    manifests.add(req);
                    lines.push('来源板：' + req.sourceBoard.id + ' / ' + req.sourceBoard.version + ' / ' + req.sourceBoard.chip);
                    lines.push('声明驱动：' + (req.driverIds.join(', ') || '无') + '；声明动作：' + (req.actionTypes.join(', ') || '无'));
                    warnings.push('来源声明未经设备核验，驱动/固件版本未提供；目标兼容性仍由完整候选预检决定');
                }
                const entry = this._validateConfigTransferEntry(file);
                if (!CONFIG_MIGRATION_FILES.includes(entry.name)) {
                    errors.push('选中配置包含整组迁移不支持的文件，请取消该项；网络、协议、用户和凭据不从来源导入');
                    continue;
                }
                if (documents.has(entry.name)) {
                    errors.push('选中配置存在重复文件名，请先消除歧义');
                    continue;
                }
                documents.set(entry.name, JSON.parse(entry.content));
            }
            const peripherals = documents.get('peripherals.json')?.peripherals;
            const rules = documents.get('periph_exec.json')?.rules;
            const indexed = Array.isArray(peripherals), ids = new Map();
            let invalid = 0, resolved = 0, missing = 0, unresolved = 0, disabled = 0, wrongType = 0;
            let incompatible = 0, selfPair = 0;
            const digitalInputs = [11,13,14,18,19,20];
            const validId = id => typeof id === 'string' && id.length > 0 && !id.includes('\0');
            if (indexed) for (const p of peripherals) {
                if (!validId(p?.id) || ids.has(p.id)) invalid++;
                else ids.set(p.id, p);
            }
            const reference = (id, expectedType, actionType) => {
                if (!validId(id)) { missing++; return; }
                // Modbus instances are synthesized from protocol.json, not
                // persisted in peripherals.json. Never call them missing here.
                if (id.startsWith('modbus:') && expectedType === undefined) { unresolved++; return; }
                if (!indexed) { unresolved++; return; }
                const target = ids.get(id);
                if (!target) { missing++; return; }
                if (expectedType !== undefined && !(Array.isArray(expectedType)
                    ? expectedType.includes(target.type) : target.type === expectedType)) { wrongType++; return; }
                if (actionType !== undefined) {
                    const compatible = this._matchesConfigTransferActionTarget(actionType, target);
                    if (compatible === false) { incompatible++; return; }
                    if (compatible === null) unresolved++;
                }
                resolved++;
                if (target.enabled !== true && target.enabled !== 1) disabled++;
            };
            if (indexed) for (const p of peripherals) {
                const type = p?.type, params = p?.params;
                if (type >= 11 && type <= 21 && params?.ioSource === 1) reference(params.providerId, 55);
                if ((type === 55 || type === 38) && params?.busPeripheralId) reference(params.busPeripheralId, 2);
                if (type === 57) { reference(params?.busPeripheralId, 2); reference(params?.oePeripheralId, 12); }
                if (type === 55 && params?.resetPeripheralId) reference(params.resetPeripheralId, 12);
                if (digitalInputs.includes(type) && params?.pairedInputId !== undefined && params.pairedInputId !== '') {
                    if (params.pairedInputId === p.id) selfPair++;
                    else reference(params.pairedInputId, digitalInputs);
                }
            }
            // Mirrors requiresPeripheralTarget in PeripheralActionContract.h.
            // This compares local metadata; server schema/admission still applies.
            const peripheralActions = new Set([0,1,2,3,4,5,10,11,12,13,14,16,17,19,24,25,26,27,28,29,30,35,36,37,38,39,40]);
            // Resolve only explicit saved action19 keys; implicit legacy fields
            // need the firmware's category resolver. Never split IDs on '_'.
            const producers = new Map();
            let implicitSources = false;
            if (Array.isArray(rules)) for (const sourceRule of rules) for (const action of Array.isArray(sourceRule?.actions) ? sourceRule.actions : []) {
                if (action?.actionType !== 19) continue;
                let cfg;
                try { cfg = JSON.parse(action.actionValue); } catch (_) { implicitSources = true; continue; }
                const owner = cfg?.periphId || action.targetPeriphId, field = cfg?.channelField || cfg?.dataField;
                if (!validId(owner) || !validId(field)) { implicitSources = true; continue; }
                const key = owner + '_' + field, entries = producers.get(key) || [];
                entries.push(sourceRule); producers.set(key, entries);
            }
            if (Array.isArray(rules)) for (const rule of rules) {
                if (!Array.isArray(rule?.actions)) { unresolved++; continue; }
                for (const action of rule.actions) {
                    if (peripheralActions.has(action?.actionType)) reference(action.targetPeriphId, undefined, action.actionType);
                    else if (![6,7,8,9,31,32].includes(action?.actionType)) unresolved++;
                    if (action?.actionType === 40) {
                        if (rule.enabled !== false && rule.enabled !== 0) errors.push('SD记录规则必须明确禁用后导入');
                        let cfg;
                        try { cfg = JSON.parse(action.actionValue); } catch (_) {}
                        if (typeof cfg?.sourceRef !== 'string' || !/^[A-Za-z0-9_.:-]{1,96}$/.test(cfg.sourceRef)) {
                            errors.push('SD记录来源标识无效'); continue;
                        }
                        const matches = producers.get(cfg.sourceRef) || [];
                        if (matches.length > 1) errors.push('SD记录来源对应多个采集动作，须消除歧义');
                        else if (!matches.length && !implicitSources) errors.push('SD记录来源在所选完整规则文件中缺失');
                        else if (implicitSources) { unresolved++; warnings.push('SD记录来源含隐式通道，须由目标固件精确解析后重新核验'); }
                        else if (matches[0].enabled !== true || matches[0].recordHistory !== true)
                            warnings.push('SD记录来源规则尚未启用或未记录数值历史，启用记录器前须修正');
                    }
                }
                // Platform item IDs, event filters and poll/remote sources are
                // distinct namespaces; do not reinterpret them as peripherals.
                if (Array.isArray(rule?.triggers)) unresolved += rule.triggers.filter(t => t?.triggerType !== 1).length;
            }
            if (documents.has('peripherals.json') && !indexed) warnings.push('外设文件非标准数组结构，未建立引用索引');
            if (documents.has('periph_exec.json') && !Array.isArray(rules)) warnings.push('执行文件非标准规则数组，未解析动作引用');
            if (invalid) errors.push('外设 ID 重复或无效：' + invalid + ' 项，请修正后导入');
            if (missing) errors.push('已知引用为空或在所选外设文件中缺失：' + missing + ' 项，请补齐或修正');
            if (wrongType) errors.push('扩展器、控制引脚、I2C 总线或配对输入引用类型不符：' + wrongType + ' 项');
            if (incompatible) errors.push('所选动作与目标外设类型不兼容：' + incompatible + ' 项');
            if (selfPair) errors.push('配对输入不能引用自身：' + selfPair + ' 项');
            if (disabled) warnings.push('引用目标未显式启用：' + disabled + ' 项，不能据此认为可执行');
            if (unresolved) warnings.push('尚未解析的引用/来源：' + unresolved + ' 项（未选外设文件或属于协议、规则、事件等命名空间）');
            lines.push('所选文件内已定位引用：' + resolved + ' 项；不代表目标设备可用');
            warnings.push('类型检查仅依据所选文件；未读取目标设备，未核验命令参数、GPIO、资源、固件版本与跨文件原子性');
            return {lines, warnings, errors};
        },

        /**
         * 显示配置文件选择弹窗
         * @param {Array} items - [{name, size?}] 配置文件列表
         * @param {string} mode - 'export' 或 'import'
         * @returns {Promise<Array>} 用户选中的文件列表
         */
        async _showConfigTransferModal(items, mode, selectionPreview) {
            const self = this;
            const modal = await AppState._ensureModalForAction('config-transfer-modal');
            if (!modal) throw new Error('弹窗元素不存在');
            return new Promise((resolve, reject) => {
                const titleEl = document.getElementById('config-transfer-modal-title');
                const listEl = document.getElementById('config-transfer-list');
                const selectAllEl = document.getElementById('config-transfer-select-all');
                const summaryEl = document.getElementById('config-transfer-summary');
                const confirmBtn = document.getElementById('config-transfer-confirm-btn');
                const cancelBtn = document.getElementById('config-transfer-cancel-btn');
                const preview = document.getElementById('config-transfer-preview');
                const acknowledgement = document.getElementById('config-transfer-ack');
                const acknowledgementRow = document.getElementById('config-transfer-ack-row');
                const isImport = mode === 'import';
                if (isImport && (!preview || !acknowledgement || !acknowledgementRow)) {
                    reject(new Error('迁移预览未加载，请刷新页面后重试')); return;
                }
                if (preview) preview.classList.toggle('is-hidden', !isImport);
                if (acknowledgementRow) acknowledgementRow.classList.toggle('is-hidden', !isImport);
                if (acknowledgement) acknowledgement.checked = false;
                const closeBtn = document.querySelector('#config-transfer-modal .modal-close-btn');
                if (!listEl || !confirmBtn || !cancelBtn) { reject(new Error('弹窗元素不存在')); return; }

                // 设置标题和确认按钮文字
                if (titleEl) titleEl.textContent = mode === 'export' ? '选择导出的配置' : '选择导入的配置';
                confirmBtn.textContent = mode === 'export' ? '确认导出' : '确认导入';

                // 渲染复选框列表
                listEl.innerHTML = '';
                items.forEach((item, idx) => {
                    const label = document.createElement('label');
                    label.className = 'fb-checkbox';
                    const cb = document.createElement('input');
                    cb.type = 'checkbox';
                    cb.checked = true;
                    cb.dataset.idx = idx;
                    const span = document.createElement('span');
                    const displayName = self._getConfigTransferLabel(item.name);
                    span.textContent = displayName + (item.size ? ' (' + (item.size > 1024 ? (item.size / 1024).toFixed(1) + ' KB' : item.size + ' B') + ')' : '');
                    label.appendChild(cb);
                    label.appendChild(span);
                    listEl.appendChild(label);
                });

                // 更新摘要
                function updateSummary() {
                    const selected = Array.from(listEl.querySelectorAll('input[type="checkbox"]:checked')).map(cb => items[Number(cb.dataset.idx)]);
                    const checked = selected.length;
                    if (summaryEl) summaryEl.textContent = checked + '/' + items.length + ' 项';
                    let dependencies = null;
                    if (isImport && selectionPreview) {
                        try {
                            dependencies = selectionPreview(selected);
                            if (!dependencies || !['lines','warnings','errors'].every(key => Array.isArray(dependencies[key]))) throw new Error();
                        } catch (_) {
                            dependencies = {lines: [], warnings: [], errors: ['引用预览无法完成，禁止继续导入']};
                        }
                    }
                    const ready = !isImport || (selected.every(item => item?.preview && Array.isArray(item.preview.lines) && Array.isArray(item.preview.warnings)) && !dependencies?.errors.length);
                    confirmBtn.disabled = checked === 0 || !ready || (isImport && !acknowledgement.checked);
                    if (isImport) {
                        preview.replaceChildren();
                        const summary = document.createElement('p');
                        summary.textContent = '所选配置将先暂存到目标设备，再校验完整依赖和资源；确认后等待下次重启整组应用。目标身份、网络与凭据保持不变。此处尚未进行设备预检或接线核验。';
                        preview.appendChild(summary);
                        if (dependencies) {
                            const dependencySummary = document.createElement('p');
                            dependencySummary.textContent = dependencies.lines.concat(dependencies.errors, dependencies.warnings).join('；');
                            preview.appendChild(dependencySummary);
                        }
                        selected.forEach(item => {
                            const section = document.createElement('p');
                            section.textContent = self._getConfigTransferLabel(item.name) + '：' + (item.preview
                                ? item.preview.lines.concat(item.preview.warnings).join('；') : '缺少内容预览，禁止继续');
                            preview.appendChild(section);
                        });
                    }
                    if (selectAllEl) selectAllEl.checked = checked === items.length;
                }
                updateSummary();

                // 全选/取消全选
                function onSelectAll() {
                    const checked = selectAllEl.checked;
                    listEl.querySelectorAll('input[type="checkbox"]').forEach(cb => { cb.checked = checked; });
                    if (acknowledgement) acknowledgement.checked = false;
                    updateSummary();
                }
                if (selectAllEl) { selectAllEl.checked = true; selectAllEl.addEventListener('change', onSelectAll); }

                // 单项变化
                function onItemChange() { if (acknowledgement) acknowledgement.checked = false; updateSummary(); }
                listEl.addEventListener('change', onItemChange);
                if (acknowledgement) acknowledgement.addEventListener('change', updateSummary);

                // 清理函数
                function cleanup() {
                    if (selectAllEl) selectAllEl.removeEventListener('change', onSelectAll);
                    listEl.removeEventListener('change', onItemChange);
                    if (acknowledgement) acknowledgement.removeEventListener('change', updateSummary);
                    confirmBtn.removeEventListener('click', onConfirm);
                    cancelBtn.removeEventListener('click', onCancel);
                    if (closeBtn) closeBtn.removeEventListener('click', onCancel);
                    AppState.hideModal('config-transfer-modal');
                }

                function onConfirm() {
                    updateSummary();
                    if (confirmBtn.disabled || (isImport && !acknowledgement.checked)) return;
                    const selected = [];
                    listEl.querySelectorAll('input[type="checkbox"]:checked').forEach(cb => {
                        const idx = parseInt(cb.dataset.idx);
                        if (items[idx]) selected.push(items[idx]);
                    });
                    cleanup();
                    resolve(selected);
                }
                function onCancel() {
                    cleanup();
                    reject(new Error('用户取消'));
                }

                confirmBtn.addEventListener('click', onConfirm);
                cancelBtn.addEventListener('click', onCancel);
                if (closeBtn) closeBtn.addEventListener('click', onCancel);
                AppState.showModal('config-transfer-modal');
            });
        },

        _configTransferByteLength(content) {
            let bytes = 0;
            for (let i = 0; i < content.length;) {
                const code = content.codePointAt(i);
                bytes += code <= 0x7F ? 1 : (code <= 0x7FF ? 2 : (code <= 0xFFFF ? 3 : 4));
                i += code > 0xFFFF ? 2 : 1;
            }
            return bytes;
        },

        _splitConfigTransferChunks(content, maxBytes) {
            const chunks = [];
            let start = 0;
            let bytes = 0;
            for (let i = 0; i < content.length;) {
                const code = content.codePointAt(i);
                const charLen = code > 0xFFFF ? 2 : 1;
                const charBytes = code <= 0x7F ? 1 : (code <= 0x7FF ? 2 : (code <= 0xFFFF ? 3 : 4));
                if (bytes > 0 && bytes + charBytes > maxBytes) {
                    chunks.push(content.slice(start, i));
                    start = i;
                    bytes = 0;
                }
                bytes += charBytes;
                i += charLen;
            }
            if (start < content.length) chunks.push(content.slice(start));
            return chunks;
        },

        _validateConfigTransferEntry(item) {
            const name = this._normalizeConfigFileName(item && item.name);
            const content = String((item && item.content) || '');
            if (!/^[A-Za-z0-9_.-]+\.json$/.test(name)) {
                throw new Error('配置文件名不正确：' + (item && item.name ? item.name : '--'));
            }
            if (!content || this._configTransferByteLength(content) > MAX_CONFIG_TRANSFER_BYTES) {
                throw new Error('配置文件过大或为空：' + name);
            }
            try {
                JSON.parse(content);
            } catch (err) {
                throw new Error('配置文件不是有效 JSON：' + name);
            }
            return { name, content };
        },



        _readReusablePlanRequirements(bundle) {
            const req = bundle.requirements, board = req?.sourceBoard;
            const identifier = value => typeof value === 'string' && /^[A-Za-z0-9_.-]{1,96}$/.test(value);
            if (bundle.version !== 1 || !board || ![board.id, board.version, board.chip].every(identifier) ||
                !Array.isArray(req.driverIds) || req.driverIds.length > 64 || !req.driverIds.every(identifier) ||
                !Array.isArray(req.actionTypes) || req.actionTypes.length > 64 ||
                !req.actionTypes.every(value => Number.isInteger(value) && value >= 0 && value <= 255) ||
                ['targetPreflightRequired','wiringRemapRequired','peripheralsAndRulesDisabled'].some(key => req[key] !== true)) {
                throw new Error('可复用方案版本或来源要求缺失/无效，请重新导出方案');
            }
            // Copy only reviewed metadata, never retain arbitrary imported fields.
            return {sourceBoard: {id: board.id, version: board.version, chip: board.chip},
                driverIds: [...new Set(req.driverIds)], actionTypes: [...new Set(req.actionTypes)]};
        },

        _readSelectedConfigEntries(selectedFiles) {
            return Promise.all(selectedFiles.map((file) => file.text().then((text) => ({ file, text }))))
                .then((items) => {
                    const entries = [];
                    let requiresBoardMigration = false;
                    items.forEach(({ file, text }) => {
                        let parsed = null;
                        try {
                            parsed = JSON.parse(text);
                        } catch (err) {
                            parsed = null;
                        }

                        if (parsed && parsed.type === 'fastbee-config-bundle' && Array.isArray(parsed.files)) {
                            let planRequirements;
                            if (parsed.scope === 'reusable-plan') {
                                planRequirements = this._readReusablePlanRequirements(parsed);
                                requiresBoardMigration = true;
                            }
                            parsed.files.forEach((item) => entries.push({...this._validateConfigTransferEntry(item), planRequirements}));
                            return;
                        }

                        // 自动检测：文件名不是已知配置名时，根据 JSON 内容推断正确的配置文件名
                        const normalizedName = this._normalizeConfigFileName(file.name);
                        const detectedName = this._detectConfigFileName(normalizedName, parsed);
                        if (detectedName !== normalizedName) {
                            // 在页面上显示自动重命名提示
                            this._setConfigTransferStatus(
                                '已自动识别：' + file.name + ' → ' + this._getConfigTransferLabel(detectedName)
                            );
                        }

                        // JSON 格式预检查
                        if (!parsed || typeof parsed !== 'object') {
                            throw new Error('文件' + ' ' + file.name + ' ' + '不是有效的 JSON 格式');
                        }

                        entries.push({
                            name: detectedName,
                            content: text
                        });
                    });

                    const validated = entries.map((item) => ({...this._validateConfigTransferEntry(item),
                        ...(item.planRequirements ? {planRequirements: item.planRequirements} : {})}));
                    if (requiresBoardMigration) validated.requiresBoardMigration = true;
                    return validated;
                });
        },

        _buildReusableConfigPlan(entries, catalog) {
            const profile = catalog?.profiles?.find(item => item.id === catalog.activeProfileId && item.active === true);
            if (catalog?.activeProfileSupported !== true || !profile?.version || typeof profile.id !== 'string' ||
                typeof catalog.compiledChip !== 'string') throw new Error('无法确认来源板型与版本，未生成方案');
            if (entries.some(entry => !CONFIG_PLAN_FILES.includes(entry.name))) throw new Error('方案包含不允许公开的配置类别');
            const model = this._boardMigrationModel(entries), actions = new Set(), drivers = new Set();
            const secretKeys = new Set(['authcode','apikey','authheader','authorization','bearertoken','token','accesstoken','refreshtoken',
                'clientid','deviceid','devicenum','mqttsecret','password','passwordhash','salt','stapassword','appassword',
                'username','userid','wifipassword','secret','clientsecret','privatekey']);
            let visited = 0, removed = 0;
            const scrub = (value, depth = 0) => {
                if (++visited > 20000 || depth > 32) throw new Error('方案结构超出公开导出上限');
                if (!value || typeof value !== 'object') return;
                for (const key of Object.keys(value)) {
                    if (secretKeys.has(key.toLowerCase().replace(/[^a-z0-9]/g, ''))) { value[key] = ''; removed++; }
                    else scrub(value[key], depth + 1);
                }
            };
            const rules = model.documents.get('periph_exec.json')?.rules || [];
            for (const rule of rules) {
                if (rule.scriptContent) throw new Error('含命令脚本文本的执行配置不能直接公开，请取消选择执行文件或改用类型化动作');
                for (const action of rule.actions || []) {
                    if (!Number.isInteger(action.actionType) || [10,15,29].includes(action.actionType))
                        throw new Error('含通用发送或命令脚本的执行配置不能直接公开，请取消选择执行文件或改用类型化动作');
                    actions.add(action.actionType);
                    // Structured action values are serialized JSON inside JSON;
                    // recurse into them rather than treating them as plain text.
                    if (typeof action.actionValue === 'string' && /^\s*[\[{]/.test(action.actionValue)) {
                        const payload = JSON.parse(action.actionValue); scrub(payload); action.actionValue = JSON.stringify(payload);
                    }
                }
            }
            for (const peripheral of model.documents.get('peripherals.json').peripherals) {
                if (typeof peripheral.params.driver === 'string' && peripheral.params.driver) drivers.add(peripheral.params.driver);
            }
            const files = Array.from(model.documents, ([name, value]) => {
                scrub(value); return {name, content: JSON.stringify(value)};
            });
            if (files.reduce((sum, file) => sum + this._configTransferByteLength(file.content), 0) > 128 * 1024)
                throw new Error('可复用方案最多128KB，请减少选择项');
            const dependencies = this._buildConfigTransferDependencyPreview(files);
            if (dependencies.errors.length) throw new Error(dependencies.errors.join('；'));
            return {type: 'fastbee-config-bundle', version: 1, scope: 'reusable-plan', sanitized: true,
                sanitization: {policy: 'known-credential-fields-v1', clearedFields: removed, customTextReviewRequired: true},
                requirements: {sourceBoard: {id: profile.id, version: profile.version, chip: catalog.compiledChip},
                    driverIds: Array.from(drivers).sort(), actionTypes: Array.from(actions).sort((a,b) => a-b),
                    targetPreflightRequired: true, wiringRemapRequired: true, peripheralsAndRulesDisabled: true,
                    excluded: ['device.json','network.json','protocol.json','users.json','auth.json','rule_scripts.json','ir_keys.json']}, files};
        },

        async exportDeviceConfigBundle(reusable = false) {
            const btn = document.getElementById(reusable === true ? 'dev-config-plan-export-btn' : 'dev-config-export-btn');
            if (btn) btn.disabled = true;
            this._setConfigTransferStatus('正在读取配置列表...');

            return apiGetFresh('/api/config/transfer/list')
                .then((res) => {
                    let files = (res && res.success && res.data && res.data.files) ? res.data.files : [];
                    if (reusable === true) files = files.filter(file => CONFIG_PLAN_FILES.includes(file.name));
                    if (!files.length) throw new Error('没有可导出的配置文件');
                    return this._showConfigTransferModal(files, 'export');
                })
                .then(async (selected) => {
                    if (!selected.length) throw new Error('未选择任何配置文件');
                    this._setConfigTransferStatus('正在导出...');

                    let bundle = {
                        type: 'fastbee-config-bundle',
                        version: 1,
                        scope: selected.length === 1 ? selected[0].name : 'selected',
                        exportedAt: new Date().toISOString(),
                        files: []
                    };
                    const rawContents = [];

                    for (let i = 0; i < selected.length; i++) {
                        const item = selected[i];
                        this._setConfigTransferStatus('正在导出' + ' ' + item.name + ' (' + (i + 1) + '/' + selected.length + ')...');
                        const url = new URL('/api/config/transfer/export', window.location.origin);
                        url.searchParams.set('name', item.name);
                        const resp = await this._configTransferFetch(url.toString());
                        if (!resp.ok) throw new Error('导出失败：' + item.name);
                        const content = await resp.text();
                        if (this._configTransferByteLength(content) > MAX_CONFIG_TRANSFER_BYTES) {
                            throw new Error('配置文件过大：' + item.name);
                        }
                        bundle.files.push({ name: item.name, content });
                        rawContents.push(content);
                    }

                    if (reusable === true) {
                        const profile = await apiGetFresh('/api/boards/profiles');
                        if (profile?.success !== true) throw new Error('来源板型查询失败，未生成方案');
                        bundle = this._buildReusableConfigPlan(bundle.files, profile.data);
                        // Detect concurrent edits without assuming that separate
                        // export requests form an atomic server-side snapshot.
                        for (const original of selected) {
                            const first = selected.indexOf(original);
                            const response = await this._configTransferFetch('/api/config/transfer/export?name=' + encodeURIComponent(original.name));
                            if (!response.ok || await response.text() !== rawContents[first]) throw new Error('导出期间配置发生变化，未生成方案；请稳定配置后重试');
                        }
                        if (!await FBConfirm({ title: '导出迁移方案', message: '方案不包含设备身份、网络、协议、用户、认证、转换脚本或红外文件，标准凭据字段已清空。\n逻辑ID、名称、静态文本和数据模板仍会保留，请确认其中没有现场秘密。\n所有外设与执行规则将停用；目标板须重新映射接线并完成预检。确认下载？', danger: true, requireCheck: true, checkLabel: '我已确认方案中没有现场秘密' })) { this._setConfigTransferStatus('--'); return; }
                    }

                    // 单文件直接导出原始JSON，多文件导出bundle包
                    const exportContent = reusable !== true && selected.length === 1
                        ? bundle.files[0].content
                        : JSON.stringify(bundle, null, 2);
                    const exportName = reusable === true ? 'fastbee-plan-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json' : selected.length === 1
                        ? bundle.files[0].name
                        : 'fastbee-config-' + new Date().toISOString().replace(/[:.]/g, '-') + '.json';
                    const blob = new Blob([exportContent], { type: 'application/json' });
                    const link = document.createElement('a');
                    const objectUrl = URL.createObjectURL(blob);
                    link.href = objectUrl;
                    link.download = exportName;
                    document.body.appendChild(link);
                    link.click();
                    link.remove();
                    setTimeout(() => URL.revokeObjectURL(objectUrl), 3000);

                    this._setConfigTransferStatus('配置导出完成：' + bundle.files.length + ' 个文件');
                    Notification.success('配置导出完成', '配置导入/导出');
                })
                .catch((err) => {
                    if (err && err.message === '用户取消') {
                        this._setConfigTransferStatus('--');
                        return;
                    }
                    console.error('[device-config] export config bundle failed:', err);
                    this._setConfigTransferStatus('配置导出失败');
                    Notification.error(err && err.message ? err.message : '配置导出失败', '配置导入/导出');
                })
                .finally(() => {
                    if (btn) btn.disabled = false;
                });
        },

        _boardMigrationModel(entries) {
            const documents = new Map();
            for (const entry of entries) {
                if (!CONFIG_MIGRATION_FILES.includes(entry.name) || documents.has(entry.name)) throw new Error('换板方案包含不支持或重复的文件：' + entry.name);
                documents.set(entry.name, JSON.parse(entry.content));
            }
            const peripherals = documents.get('peripherals.json')?.peripherals;
            if (!Array.isArray(peripherals)) throw new Error('换板需要完整的外设配置，请选择 peripherals.json');
            const slots = [], paused = [];
            const addPin = (object, key, label) => {
                const value = object[key];
                if (!Number.isInteger(value) || value < -1 || value > 255) throw new Error('接线字段无效：' + label);
                if (value !== -1 && value !== 255) slots.push({object, key, pin: value, label});
            };
            const screenPins = ['mosi','sclk','cs','dc','rst','backlight','touchSda','touchScl','touchInt','touchRst',
                'rgbDe','rgbVsync','rgbHsync','rgbPclk','controlSda','controlScl'];
            for (const peripheral of peripherals) {
                if (!peripheral || typeof peripheral.id !== 'string' || !peripheral.id ||
                    !Array.isArray(peripheral.pins) || !peripheral.params || typeof peripheral.enabled !== 'boolean') {
                    throw new Error('外设结构不完整，不能重映射');
                }
                const label = (peripheral.name || peripheral.id) + ' [' + peripheral.id + ']';
                peripheral.pins.forEach((_, index) => addPin(peripheral.pins, index, label + ' / pins[' + index + ']'));
                if (peripheral.params.pins !== undefined) {
                    const pins = peripheral.params.pins;
                    if (peripheral.type !== 56 || !pins || Array.isArray(pins) || typeof pins !== 'object') throw new Error('不支持的命名接线：' + label);
                    for (const key of Object.keys(pins)) {
                        if (key === 'rgbDataPins') {
                            if (!Array.isArray(pins[key]) || pins[key].length !== 16) throw new Error('RGB数据接线必须为16路：' + label);
                            pins[key].forEach((_, index) => addPin(pins[key], index, label + ' / rgbDataPins[' + index + ']'));
                        } else {
                            if (!screenPins.includes(key)) throw new Error('未知接线字段：' + label + ' / ' + key);
                            addPin(pins, key, label + ' / ' + key);
                        }
                    }
                }
                if (peripheral.enabled) paused.push(label);
                peripheral.enabled = false;
            }
            for (const name of ['periph_exec.json', 'rule_scripts.json']) {
                const config = documents.get(name);
                if (config === undefined) continue;
                if (!config || !Array.isArray(config.rules)) throw new Error('规则结构不完整：' + name);
                for (const rule of config.rules) {
                    if (!rule || typeof rule.enabled !== 'boolean') throw new Error('规则缺少明确启用状态：' + name);
                    if (rule.enabled) paused.push(name + ' / ' + (rule.name || rule.id));
                    rule.enabled = false;
                    if (name !== 'periph_exec.json') continue;
                    for (const action of rule.actions || []) {
                        if (Number(action.actionType) === 15 && /\b(?:GPIO|PWM|DAC)\s+/i.test(action.actionValue || rule.scriptContent || '')) {
                            throw new Error('规则“' + (rule.name || rule.id) + '”含直接GPIO命令；换板前请在规则表单改用逻辑外设动作');
                        }
                        // Legacy/custom payloads may embed wiring outside the
                        // peripheral schema. Never silently rewrite JSON text.
                        if (typeof action.actionValue === 'string' && /^\s*\{/.test(action.actionValue)) {
                            const value = JSON.parse(action.actionValue);
                            const hasWiring = object => object && typeof object === 'object' && Object.keys(object).some(key =>
                                /^(?:pins?|gpio|trigPin|echoPin|txPin|rxPin|dePin)$/i.test(key) || hasWiring(object[key]));
                            if (hasWiring(value)) throw new Error('规则“' + (rule.name || rule.id) + '”含旧接线参数，请先用动作表单重新保存');
                        }
                    }
                }
            }
            const groups = new Map();
            for (const slot of slots) {
                if (!groups.has(slot.pin)) groups.set(slot.pin, []);
                groups.get(slot.pin).push(slot.label);
            }
            return {documents, slots, paused, groups};
        },

        _applyBoardMigrationPins(model, mapping, profile) {
            if (!profile || !Array.isArray(profile.pins)) throw new Error('目标板引脚信息不完整');
            const used = new Set();
            for (const source of model.groups.keys()) {
                const target = mapping.get(source);
                const pin = profile.pins.find(item => item.gpio === target);
                if (!Number.isInteger(target) || !pin || !['recommended', 'conditional'].includes(pin.policy)) throw new Error('GPIO ' + source + ' 尚未映射到可用目标引脚');
                if (used.has(target)) throw new Error('不同源GPIO不能合并到同一个目标引脚：' + target);
                used.add(target);
            }
            // Validate the whole mapping before changing even the local draft.
            for (const slot of model.slots) slot.object[slot.key] = mapping.get(slot.pin);
            return Array.from(model.documents, ([name, document]) => ({name, content: JSON.stringify(document)}));
        },

        async _reviewBoardMigration(entries) {
            const response = await apiGetFresh('/api/boards/profiles'), catalog = response?.data;
            const profile = catalog?.profiles?.find(item => item.id === catalog.activeProfileId && item.active === true);
            if (response?.success !== true || catalog?.activeProfileSupported !== true || !profile ||
                !Array.isArray(profile.pins) || !profile.version) throw new Error('目标固件没有可核验的活动板型，不能换板迁移');
            const files = entries.map(entry => ({...entry}));
            const baselines = [];
            // Preserve target-only rules but pause them too: an old target
            // trigger must not start driving a newly mapped peripheral.
            const listed = await apiGetFresh('/api/config/transfer/list');
            if (listed?.success !== true || !Array.isArray(listed.data?.files)) throw new Error('无法核验目标规则文件，未开始迁移');
            for (const name of ['periph_exec.json', 'rule_scripts.json']) {
                if (files.some(file => file.name === name)) continue;
                if (!listed.data.files.some(file => file.name === name)) { baselines.push({name, content: ''}); continue; }
                const saved = await this._configTransferFetch('/api/config/transfer/export?name=' + name);
                if (!saved.ok) throw new Error('无法保全目标规则：' + name);
                const original = this._validateConfigTransferEntry({name, content: await saved.text()});
                files.push(original); baselines.push(original);
            }
            const model = this._boardMigrationModel(files);
            const modal = await AppState._ensureModalForAction('config-board-migration-modal');
            if (!modal) throw new Error('换板接线表单未加载，请刷新页面');
            return new Promise((resolve, reject) => {
                const field = name => document.getElementById('config-board-' + name);
                const list = field('pins'), status = field('status'), ack = field('ack');
                const save = field('confirm'), cancel = field('cancel'), title = field('profile');
                const close = modal.querySelector('.modal-close-btn');
                if (![list,status,ack,save,cancel,title,close].every(Boolean)) { reject(new Error('换板接线表单不完整')); return; }
                title.textContent = '目标：' + profile.displayName + ' / ' + profile.version + ' / ' + catalog.compiledChip +
                    '。板型策略不能代替实物接线核验。源板引脚编号不会自动套用到目标板。';
                list.replaceChildren(); ack.checked = false; save.disabled = true;
                const rows = [];
                for (const [pin, labels] of Array.from(model.groups).sort((a,b) => a[0]-b[0])) {
                    const row = document.createElement('label'); row.className = 'fb-form-group';
                    const text = document.createElement('span'); text.textContent = '源 GPIO ' + pin + ' · ' + labels.join('；');
                    const select = document.createElement('select');
                    const empty = document.createElement('option'); empty.value = ''; empty.textContent = '请选择目标GPIO'; select.appendChild(empty);
                    for (const choice of profile.pins.filter(item => ['recommended','conditional'].includes(item.policy))) {
                        const option = document.createElement('option'); option.value = String(choice.gpio);
                        option.textContent = 'GPIO ' + choice.gpio + (choice.capabilities?.output === false ? ' · 仅输入' : '') +
                            (choice.risks?.length ? ' · ' + choice.risks.join(', ') : '');
                        select.appendChild(option);
                    }
                    row.appendChild(text); row.appendChild(select); list.appendChild(row); rows.push({pin,select});
                }
                const read = () => new Map(rows.filter(row => row.select.value !== '').map(row => [row.pin, Number(row.select.value)]));
                const update = event => {
                    if (event?.target !== ack) ack.checked = false;
                    try {
                        this._applyBoardMigrationPins(model, read(), profile);
                        status.textContent = '接线已填写。' + model.paused.length + '项原启用配置将停用；所有外设及规则须在重启后单独复核再启用。' +
                            '下一步只暂存并预检，不重启、不启用。目标现有规则如未选入，会保留内容并停用。';
                        save.disabled = !ack.checked;
                    } catch (error) { status.textContent = error.message; save.disabled = true; }
                };
                const cleanup = () => {
                    list.removeEventListener('change', update); ack.removeEventListener('change', update);
                    save.removeEventListener('click', confirm); cancel.removeEventListener('click', abort); close.removeEventListener('click', abort);
                    AppState.hideModal('config-board-migration-modal');
                };
                const abort = () => { cleanup(); reject(new Error('用户取消')); };
                const confirm = () => {
                    if (save.disabled || !ack.checked) return;
                    try { const result = this._applyBoardMigrationPins(model, read(), profile); cleanup(); resolve({files: result, baselines}); }
                    catch (error) { status.textContent = error.message; save.disabled = true; }
                };
                list.addEventListener('change', update); ack.addEventListener('change', update);
                save.addEventListener('click', confirm); cancel.addEventListener('click', abort); close.addEventListener('click', abort);
                update(); AppState.showModal('config-board-migration-modal');
            });
        },

        _configMigrationStatusText(data) {
            const labels = {
                empty: '没有待处理的配置迁移', receiving: '配置正在暂存，尚未安排应用',
                prepared: '整组配置已准备，等待下次重启应用；重启前请核验接线与恢复条件',
                applying: '配置恢复进行中，请勿再次提交', committed: '整组配置已在启动时应用；运行状态和实物效果仍须核验',
                rolled_back: '上次迁移未完成，已恢复整组旧配置；原暂存证据仍保留',
                conflict: '目标配置或固件已变化，迁移未应用；请重新核对',
                invalid: '暂存内容校验失败，迁移未应用；原暂存证据仍保留',
                blocked: '迁移恢复受阻，须保全并修复日志，不能继续导入',
                incomplete: '迁移记录不完整，须先核验暂存状态', discarding: '暂存清理未完成，可再次确认清理'
            };
            if (!data || !Object.prototype.hasOwnProperty.call(labels, data.phase)) throw new Error('迁移状态响应不完整');
            return labels[data.phase] + (data.canQuarantine === true ? '；可使用“处理暂存”保全未应用的残缺记录' : '') +
                (data.orphanRetained === true ? '；另有一份残缺暂存证据已保全' : '');
        },

        async _readConfigMigration() {
            const response = await apiGetFresh('/api/config/migration');
            if (response?.success !== true || !response.data) throw new Error('无法取得迁移状态，请检查目标固件和开发者模式');
            this._configMigrationStatusText(response.data);
            return response.data;
        },

        async checkConfigMigration() {
            try { this._setConfigTransferStatus(this._configMigrationStatusText(await this._readConfigMigration())); }
            catch (error) { this._setConfigTransferStatus(error.message || '迁移状态查询失败'); }
        },

        async discardConfigMigration() {
            if (this._configMigrationBusy) return;
            this._configMigrationBusy = true;
            try {
                const state = await this._readConfigMigration();
                if (state.phase === 'empty') { this._setConfigTransferStatus(this._configMigrationStatusText(state)); return; }
                if (state.phase === 'incomplete' && state.canQuarantine === true && /^[a-f0-9]{64}$/.test(state.orphanRevision || '')) {
                    if (!await FBConfirm({ title: '保全残缺迁移记录', message: '该暂存记录不完整，尚未进入应用阶段。确认将整份残缺记录保全到独立目录？\n不删除原证据、不改变当前配置、不重启；保全后可重新开始迁移。' })) return;
                    const result = await apiPost('/api/config/migration/quarantine', {revision: state.orphanRevision, confirm: 'QUARANTINE'}, 30000);
                    if (result?.success !== true || result.data?.phase !== 'empty' || result.data?.orphanRetained !== true)
                        throw new Error('保全结果未确认，请查询迁移状态，未自动重试');
                    this._setConfigTransferStatus('残缺暂存已整份保全，原证据未删除；可重新开始迁移');
                    return;
                }
                if (!/^[a-f0-9]{32}$/.test(state.session || '') || ['blocked', 'applying', 'incomplete'].includes(state.phase)) {
                    throw new Error('当前记录不能安全清理，请保全后修复');
                }
                if (!await FBConfirm({ title: '清除迁移暂存', message: this._configMigrationStatusText(state) + '\n确认清除该次迁移暂存内容和恢复副本？未应用的迁移将取消，当前配置保持不变。', danger: true })) return;
                const result = await apiPost('/api/config/migration/discard', {session: state.session, confirm: 'DISCARD'}, 30000);
                if (result?.success !== true || result.data?.phase !== 'empty') throw new Error('清理结果未确认，请查询迁移状态，未自动重试');
                this._setConfigTransferStatus('该次迁移的暂存和恢复副本已清理，当前配置保持不变');
            } catch (error) { this._setConfigTransferStatus(error.message || '清理结果未确认，请查询迁移状态'); }
            finally { this._configMigrationBusy = false; }
        },

        async _importMigrationBundle(entries, baselines = []) {
            const names = new Set();
            for (const entry of entries) {
                if (!CONFIG_MIGRATION_FILES.includes(entry.name) || names.has(entry.name)) throw new Error('整组迁移包含不支持或重复的文件，未开始暂存');
                names.add(entry.name);
            }
            if (!names.size) throw new Error('未选择迁移文件');
            const current = await this._readConfigMigration();
            if (current.phase !== 'empty') throw new Error(this._configMigrationStatusText(current) + '；请先查询并处理该记录');
            const files = entries.map(entry => ({name: entry.name, content: entry.content}));
            const device = files.find(entry => entry.name === 'device.json');
            if (device) {
                const response = await this._configTransferFetch('/api/config/transfer/export?name=device.json');
                if (!response.ok) throw new Error('无法读取目标设备身份，未开始迁移');
                const originalText = await response.text();
                if (this._configTransferByteLength(originalText) > 24576) throw new Error('目标设备配置超出迁移容量');
                const target = JSON.parse(originalText), source = JSON.parse(device.content);
                if (!target || Array.isArray(target) || typeof target !== 'object' || !source || Array.isArray(source)) throw new Error('设备配置格式不正确');
                for (const key of ['deviceName', 'location', 'description', 'enableNTP', 'ntpServer1', 'ntpServer2', 'timezone', 'syncInterval', 'logLevel', 'cacheDuration']) {
                    if (Object.prototype.hasOwnProperty.call(source, key)) target[key] = source[key];
                }
                device.content = JSON.stringify(target);
            }
            const selection = files.map(file => ({name: file.name, size: this._configTransferByteLength(file.content)}));
            if (selection.reduce((total, file) => total + file.size, 0) > 128 * 1024) throw new Error('整组配置最多128KB，请减少选择项后重新预览');
            const begin = await apiPost('/api/config/migration/begin', {files: JSON.stringify(selection)}, 30000);
            const session = begin?.data?.session;
            if (begin?.success !== true || begin.data?.phase !== 'receiving' || !/^[a-f0-9]{32}$/.test(session || '')) throw new Error('暂存会话未确认，请查询迁移状态，未自动重试');
            for (const original of baselines) {
                const total = this._configTransferByteLength(original.content);
                const chunks = total ? this._splitConfigTransferChunks(original.content, 1024) : [''];
                let offset = 0;
                for (const content of chunks) {
                    const checked = offset + this._configTransferByteLength(content);
                    const response = await apiPost('/api/config/migration/check-original', {
                        session, name: original.name, offset: String(offset), total: String(total), content
                    }, 30000);
                    const data = response?.data;
                    if (response?.success !== true || data?.session !== session || data?.name !== original.name ||
                        data?.matched !== true || data?.total !== total || data?.checked !== checked) {
                        throw new Error('目标原有规则与预览基线不一致，未安排迁移；请查询并清理暂存后重新预览');
                    }
                    offset = checked;
                }
            }
            for (const file of files) {
                let position = 0, offset = 0;
                while (position < file.content.length) {
                    let end = Math.min(position + 1024, file.content.length);
                    const last = file.content.charCodeAt(end - 1);
                    if (last >= 0xD800 && last <= 0xDBFF && end < file.content.length) --end;
                    const content = file.content.slice(position, end), bytes = this._configTransferByteLength(content);
                    this._setConfigTransferStatus('正在暂存 ' + file.name + '：' + offset + ' 字节');
                    const part = await apiPost('/api/config/migration/chunk', {session, name: file.name, offset: String(offset), content}, 30000);
                    if (part?.success !== true || part.data?.session !== session || part.data?.name !== file.name || part.data?.received !== offset + bytes) {
                        throw new Error('暂存分片结果未确认，请查询迁移状态，未自动重试');
                    }
                    position = end; offset += bytes;
                }
            }
            const response = await apiPost('/api/config/migration/preview', {session}, 30000), preview = response?.data;
            if (response?.success !== true || preview?.session !== session || preview?.phase !== 'reviewed' ||
                !/^[a-f0-9]{64}$/.test(preview.revision || '') || preview.activation !== 'next_boot') throw new Error('整组预检响应未确认，未安排应用');
            const issuesText = Array.isArray(preview.issues) ? preview.issues.map(issue =>
                String(issue.ownerId || '') + ' · ' + String(issue.code || '') + '：' + String(issue.message || '')).join('\n') : '';
            if (preview.valid !== true) throw new Error('目标设备拒绝迁移：' + (preview.error || '依赖或资源不兼容') +
                (issuesText ? '\n' + issuesText : '') + '；暂存保留，尚未安排应用');
            let acknowledgements = 0;
            const riskBits = {PIN_STRAPPING_ACK_REQUIRED: 1, PIN_BOARD_RESOURCE_ACK_REQUIRED: 2, PIN_UART0_ACTIVE: 4, ADC2_WIFI_ACK_REQUIRED: 8};
            if (!Array.isArray(preview.issues) || typeof preview.requiresConfirmation !== 'boolean') throw new Error('资源预检不完整，未安排应用');
            for (const issue of preview.issues) {
                if (issue.severity !== 'warning-requires-confirmation') continue;
                const bit = riskBits[issue.code];
                if (!bit) throw new Error('存在未支持的资源风险，请在外设页面处理后重新迁移');
                if (!(acknowledgements & bit)) {
                    if (!await FBConfirm({ title: '资源风险确认', message: '目标设备资源风险：' + String(issue.message || issue.code) + '\n确认已核对该项接线和恢复条件？', danger: true })) throw new Error('资源风险未确认，暂存保留，尚未安排应用');
                    acknowledgements |= bit;
                }
            }
            if (preview.requiresConfirmation && !acknowledgements) throw new Error('资源确认项缺失，未安排应用');
            if (preview.issues.some(issue => ['DORMANT_GPIO_CONFLICT', 'DORMANT_RESOURCE_CONFLICT'].includes(issue.code))) {
                throw new Error('配置启用后存在资源冲突，请清理本次暂存并修正接线后重新预览：\n' + issuesText);
            }
            if (issuesText && !await FBConfirm({ title: '资源预检提示', message: '完整资源预检提示：\n' + issuesText + '\n确认已阅读这些限制并继续准备迁移？', danger: true })) throw new Error('未确认资源预检提示，暂存保留');
            if (!await FBConfirm({ title: '准备迁移', message: '已校验 ' + files.length + ' 个文件及目标配置依赖。\n目标设备身份、网络和凭据保持不变。确认准备整组迁移，等待下次重启应用？\n不会自动重启；接线与实物效果仍须现场核验。', danger: true })) throw new Error('未确认应用，暂存保留，可查询或清理');
            const prepared = await apiPost('/api/config/migration/prepare', {
                session, revision: preview.revision, acknowledgements: String(acknowledgements), confirm: 'PREPARE'
            }, 30000);
            if (prepared?.success !== true || prepared.data?.phase !== 'prepared' || prepared.data?.session !== session ||
                prepared.data?.revision !== preview.revision || prepared.data?.applied !== false || prepared.data?.restartRequired !== true) {
                throw new Error('准备结果未确认，可能已安排下次启动应用；请先查询迁移状态，勿直接重试或重启');
            }
            return prepared.data;
        },

        importDeviceConfigBundle(boardMigration = false) {
            if (this._configMigrationBusy) return;
            const input = document.getElementById('dev-config-import-file');
            const btn = document.getElementById('dev-config-import-btn');
            const selectedFiles = input && input.files ? Array.from(input.files) : [];
            if (!selectedFiles.length) {
                Notification.warning('请先选择配置文件或配置包', '配置导入/导出');
                return;
            }
            const totalSize = selectedFiles.reduce((sum, file) => sum + (file.size || 0), 0);
            if (totalSize > MAX_CONFIG_BUNDLE_BYTES) {
                Notification.warning('配置文件总量过大，请分批导入', '配置导入/导出');
                return;
            }

            if (btn) btn.disabled = true;
            this._configMigrationBusy = true;
            this._setConfigTransferStatus('正在解析配置文件...');

            let writing = false;
            return this._readSelectedConfigEntries(selectedFiles)
                .then((files) => {
                    if (!files.length) throw new Error('配置包内没有可导入的配置文件');
                    if (files.requiresBoardMigration === true) boardMigration = true;
                    const items = files.map(f => ({ name: f.name, size: this._configTransferByteLength(f.content), preview: this._buildConfigTransferPreview(f) }));
                    // Bind selection by entry identity, not filename: deselecting
                    // one duplicate must not silently select both documents.
                    const selectFiles = selected => files.filter((_, index) => selected.includes(items[index]));
                    // 弹窗让用户选择要导入的配置
                    return this._showConfigTransferModal(
                        items, 'import', selected => this._buildConfigTransferDependencyPreview(selectFiles(selected))
                    ).then(selectFiles);
                })
                .then(async (files) => {
                    if (!files.length) throw new Error('未选择任何配置文件');
                    let baselines = [];
                    if (boardMigration === true) ({files, baselines} = await this._reviewBoardMigration(files));
                    const dependencies = this._buildConfigTransferDependencyPreview(files);
                    if (dependencies.errors.length) throw new Error(dependencies.errors.join('；'));
                    writing = true;
                    const result = await this._importMigrationBundle(files, baselines);
                    const message = this._configMigrationStatusText(result);
                    this._setConfigTransferStatus(message);
                    Notification.success(message, '配置导入/导出');
                })
                .catch((err) => {
                    if (!writing && err && err.message === '用户取消') {
                        this._setConfigTransferStatus('--');
                        return;
                    }
                    let errMsg = err && err.message ? err.message : '配置导入失败';
                    if (writing) {
                        errMsg += '；请使用“迁移状态”核验，未自动重试、清理或重启';
                    }
                    console.error('[device-config] import config bundle failed:', err);
                    this._setConfigTransferStatus('配置导入失败：' + errMsg);
                    Notification.error(errMsg, '配置导入/导出');
                })
                .finally(() => {
                    this._configMigrationBusy = false;
                    if (writing && typeof window.apiInvalidateCache === 'function') {
                        try { window.apiInvalidateCache(); } catch (error) { console.warn('[device-config] cache invalidation failed'); }
                    }
                    if (btn) btn.disabled = false;
                });
        },

    });
})();
