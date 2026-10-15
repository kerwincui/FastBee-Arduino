/**
 * Screen panel editor and read-only browser preview.
 * Physical controls are intentionally absent from this module.
 */
(function() {
    if (typeof AppState === 'undefined') return;

    var helpers = AppState._screenPanelModelHelpers;
    function screenElement(id) { return document.getElementById(id); }
    var STORAGE_KEY = 'fastbee.screenPanel.draft.v2.' + (typeof location !== 'undefined' ? location.host : 'device');
    var HISTORY_LIMIT = 20;
    var DEFAULT_LIMITS = { maxPages: 8, maxCards: 24, maxCardsPerPage: 12 };
    var SOURCE_KINDS = helpers.SOURCE_KINDS;
    var SYSTEM_ACTION_KINDS = helpers.SYSTEM_ACTION_KINDS;
    var CARD_LABELS = helpers.CARD_LABELS;
    var CARD_DEFAULTS = helpers.CARD_DEFAULTS;
    var isSupportedCardInput = helpers.isSupportedCardInput;
    var clone = helpers.clone;
    var finiteNumber = helpers.finiteNumber;
    var clamp = helpers.clamp;
    var snap = helpers.snap;
    var safeId = helpers.safeId;
    var defaultConfig = helpers.defaultConfig;
    var normalizeConfig = helpers.normalizeConfig;
    var sanitizeConfig = helpers.sanitizeConfig;
    var unwrapList = helpers.unwrapList;
    var choiceRef = helpers.choiceRef;
    var choiceLabel = helpers.choiceLabel;
    function isDisplayPeripheral(item) {
        return Number(item && item.type) === 56 || String(item && item.type || '').toLowerCase() === 'display';
    }
    var isControlChoice = helpers.isControlChoice;
    var isWeatherChoice = helpers.isWeatherChoice;
    var rectsOverlap = helpers.rectsOverlap;
    var displayStateMeta = helpers.displayStateMeta;
    var weatherIconMarkup = helpers.weatherIconMarkup;
    var cardIconMarkup = helpers.cardIconMarkup;
    var rotationDegrees = helpers.rotationDegrees;
    var rotatedSize = helpers.rotatedSize;
    var rotateRect = helpers.rotateRect;
    var unrotateRect = helpers.unrotateRect;
    var methods = {
        _spModel: null,
        _screenPanelEventsBound: false,

        _spEnsureModel() {
            if (this._spModel) return this._spModel;
            this._spModel = {
                revision: 0,
                config: defaultConfig(),
                savedConfig: defaultConfig(),
                capabilities: {},
                application: {},
                state: { items: {} },
                choices: [],
                displays: [],
                histories: {},
                historyConfig: null,
                historyStatus: null,
                choiceCapabilities: {},
                selectedPageId: 'home',
                selectedCardId: '',
                undo: [],
                redo: [],
                dirty: false,
                draftTimer: null,
                stateTimer: null,
                pointer: null,
                orderPointer: null,
                loaded: false
            };
            return this._spModel;
        },

        _spLimits() {
            var model = this._spEnsureModel();
            var raw = model.capabilities && (model.capabilities.limits || model.capabilities);
            return {
                maxPages: finiteNumber(raw && (raw.maxPages || raw.pageLimit), DEFAULT_LIMITS.maxPages),
                maxCards: finiteNumber(raw && (raw.maxCards || raw.cardLimit), DEFAULT_LIMITS.maxCards),
                maxCardsPerPage: finiteNumber(raw && (raw.maxCardsPerPage || raw.pageCardLimit), DEFAULT_LIMITS.maxCardsPerPage)
            };
        },

        _spCurrentPage() {
            var model = this._spEnsureModel();
            return model.config.pages.find(function(page) { return page.id === model.selectedPageId; }) || model.config.pages[0];
        },

        _spCurrentCard() {
            var model = this._spEnsureModel();
            var page = this._spCurrentPage();
            return page && page.cards.find(function(card) { return card.id === model.selectedCardId; }) || null;
        },

        _spReferenceSize(page, config) {
            return helpers.referenceSize(page, config, this);
        },

        _spSetMessage(message, tone) {
            var element = screenElement('sp-page-message');
            if (!element) return;
            element.textContent = message || '';
            element.classList.toggle('fb-hidden', !message);
            element.classList.toggle('is-error', tone === 'error');
            element.classList.toggle('is-success', tone === 'success');
        },

        _spSetLayoutError(message) {
            var element = screenElement('sp-layout-error');
            if (!element) return;
            element.textContent = message || '';
            element.classList.toggle('fb-hidden', !message);
        },

        _spPersistDraftSoon() {
            var model = this._spEnsureModel();
            if (model.draftTimer) clearTimeout(model.draftTimer);
            model.draftTimer = null;
            model.flushDraft = null;
            if (!model.deviceMac) return;
            var deviceMac = model.deviceMac, storageKey = STORAGE_KEY + '.' + deviceMac;
            model.flushDraft = function() {
                if (model.draftTimer) clearTimeout(model.draftTimer);
                model.draftTimer = null;
                model.flushDraft = null;
                if (model.deviceMac !== deviceMac) return;
                try {
                    if (!model.dirty && !model.historyDirty) {
                        localStorage.removeItem(storageKey);
                        return;
                    }
                    localStorage.setItem(storageKey, JSON.stringify({
                        deviceMac: deviceMac,
                        baseRevision: model.revision,
                        baseConfig: model.savedConfig ? sanitizeConfig(model.savedConfig, model.choices) : null,
                        historyRevision: model.historyConfig && model.historyConfig.revision,
                        historyDraft: model.historyDraft,
                        config: sanitizeConfig(model.config, model.choices),
                        selectedPageId: model.selectedPageId,
                        selectedCardId: model.selectedCardId,
                        updatedAt: Date.now()
                    }));
                } catch (_) { /* A full/disabled storage area must not block device saving. */ }
            };
            model.draftTimer = setTimeout(model.flushDraft, 160);
        },

        _spFlushDraft() {
            var model = this._spModel;
            if (model && model.flushDraft) model.flushDraft();
        },

        _spClearDraft() {
            var model = this._spEnsureModel();
            if (model.draftTimer) clearTimeout(model.draftTimer);
            model.draftTimer = null;
            model.flushDraft = null;
            if (model.deviceMac) try { localStorage.removeItem(STORAGE_KEY + '.' + model.deviceMac); } catch (_) { /* ignore */ }
        },

        _spRecoverDraft() {
            var model = this._spEnsureModel(), draft = model.recoverableDraft;
            if (!draft) return;
            model.config = draft.config; model.savedConfig = draft.baseConfig; model.revision = draft.baseRevision;
            model.configConflict = draft.conflict; model.selectedPageId = draft.selectedPageId; model.selectedCardId = '';
            if (draft.historyDraft && model.historyConfig && draft.historyRevision === model.historyConfig.revision) {
                model.historyDraft = draft.historyDraft; model.historyDirty = !helpers.configEqual(model.historyDraft, model.historyConfig);
            }
            this._spMarkDirty();
            if (draft.conflict) this._spSetMessage('草稿已恢复，但与设备配置有冲突。请先导出草稿，再核对设备配置。', 'error');
        },

        _spRestoreDraft(serverConfig, revision) {
            var deviceMac = this._spEnsureModel().deviceMac;
            if (!deviceMac) return null;
            try {
                var raw = localStorage.getItem(STORAGE_KEY + '.' + deviceMac);
                if (!raw) return null;
                var draft = JSON.parse(raw);
                if (!draft || draft.deviceMac !== deviceMac || !draft.config) return null;
                var sameRevision = finiteNumber(draft.baseRevision, -1) === finiteNumber(revision, 0);
                if (!sameRevision && !draft.baseConfig) return null;
                var remote = sanitizeConfig(serverConfig), base = sameRevision ? remote : draft.baseConfig;
                var merged = helpers.mergeConfig(base, draft.config, remote);
                return {
                    config: normalizeConfig(merged.config),
                    baseConfig: normalizeConfig(merged.conflicts.length ? base : remote),
                    baseRevision: merged.conflicts.length ? draft.baseRevision : revision,
                    conflict: merged.conflicts.length ? { revision: revision, config: clone(serverConfig), paths: merged.conflicts } : null,
                    selectedPageId: draft.selectedPageId,
                    selectedCardId: draft.selectedCardId, historyRevision: draft.historyRevision, historyDraft: draft.historyDraft
                };
            } catch (_) {
                return null;
            }
        },

        _spPushUndo() {
            var model = this._spEnsureModel();
            model.undo.push(clone(model.config));
            if (model.undo.length > HISTORY_LIMIT) model.undo.shift();
            model.redo = [];
        },

        _spMarkDirty(options) {
            var model = this._spEnsureModel();
            model.config.settings.bottomNavPageIds = helpers.normalizeBottomNavPageIds(model.config.settings, model.config.pages);
            model.dirty = !helpers.configEqual(sanitizeConfig(model.config, model.choices), sanitizeConfig(model.savedConfig, model.choices));
            this._spPersistDraftSoon();
            this._spRenderDirtyState();
            if (!options || options.render !== false) this._spRenderAll();
        },

        _spMutate(callback, options) {
            this._spPushUndo();
            callback(this._spEnsureModel().config);
            this._spMarkDirty(options);
        },

        _spRenderDirtyState() {
            var model = this._spEnsureModel();
            var dirty = screenElement('sp-dirty-state');
            if (dirty) {
                dirty.textContent = (model.dirty || model.weatherDirty || model.historyDirty) ? '有未保存修改' : '已保存';
                dirty.classList.toggle('is-dirty', model.dirty || model.weatherDirty || model.historyDirty);
            }
            var undo = screenElement('sp-undo');
            var redo = screenElement('sp-redo');
            if (undo) undo.disabled = model.undo.length === 0;
            if (redo) redo.disabled = model.redo.length === 0;
        },

        _spApplyStatus() {
            var model = this._spEnsureModel();
            var meta = displayStateMeta(model.application);
            var badge = screenElement('sp-runtime-badge');
            var summary = screenElement('sp-apply-summary');
            if (badge) {
                badge.className = 'status-badge ' + meta.className;
                badge.textContent = meta.pendingRestart && meta.text !== '待重启' ? meta.text + ' · 待重启' : meta.text;
            }
            if (summary) {
                summary.textContent = meta.reason || '';
                summary.classList.toggle('fb-hidden', !meta.reason);
            }
        },

        _spApplySurfaceTheme(surface, config, page) {
            if (!surface) return;
            var settings = (config || this._spEnsureModel().config).settings;
            surface.dataset.theme = page && page.themeId || settings.themeId || 'dark';
            surface.dataset.background = page && page.background && page.background.presetId ||
                settings.background && settings.background.presetId || 'solid';
        },

        _spApplySurfaceSize(surface, page, config) {
            if (!surface || !page) return;
            var logical = this._spReferenceSize(page, config);
            var rotation = rotationDegrees(page.rotation);
            var output = rotatedSize(logical, rotation);
            surface.style.setProperty('--sp-aspect', output.width + ' / ' + output.height);
            surface.style.setProperty('--sp-logical-width', String(output.width));
            surface.style.setProperty('--sp-logical-height', String(output.height));
            surface.dataset.logicalWidth = String(logical.width);
            surface.dataset.logicalHeight = String(logical.height);
            surface.dataset.outputWidth = String(output.width);
            surface.dataset.outputHeight = String(output.height);
            surface.dataset.rotation = String(rotation);
            var canvasSize = screenElement('sp-canvas-size');
            if (canvasSize && surface.id === 'sp-editor-canvas') {
                surface.parentElement?.style.setProperty('--sp-ratio', String(output.width / output.height));
                canvasSize.textContent = '逻辑尺寸 ' + logical.width + ' × ' + logical.height +
                    (rotation ? (' · 输出 ' + output.width + ' × ' + output.height + ' · 旋转 ' + rotation + '°') : '');
            }
        },

        _spLayoutTemplatePage(page, config) {
            return helpers.layoutTemplatePage(page, config, this);
        },

        _spEnsurePageRects(page, config) {
            return helpers.ensurePageRects(page, config, this);
        },

        _spCardState(card) {
            var state = this._spEnsureModel().state || {};
            var items = state.items || {};
            var ref = card.controlRef || card.sourceRef || '';
            if (card.kind === 'bar' && card.sourceRefs && card.sourceRefs.length) {
                var match = (state.cards || []).find(function(item) {
                    return item.id === card.id && item.sourceRef === card.sourceRef &&
                        JSON.stringify((item.series || []).map(function(series) { return series.sourceRef; })) === JSON.stringify(card.sourceRefs);
                });
                if (match) return match;
                return { series: card.sourceRefs.map(function(sourceRef) {
                    return Object.assign({}, items[sourceRef] || {}, { sourceRef: sourceRef });
                }) };
            }
            return items[ref] || {};
        },

        _spFormatValue(value, precision) {
            if (value === undefined || value === null || value === '') return '--';
            var number = Number(value);
            if (Number.isFinite(number)) return number.toFixed(clamp(finiteNumber(precision, 1), 0, 6));
            return String(value);
        },


        _spApplyCardRect(element, card, page, config, heightScale) {
            var logical = this._spReferenceSize(page, config);
            var output = rotatedSize(logical, page.rotation);
            var source = card.rect || { x: 0, y: 0, w: logical.width, h: logical.height };
            var rect = rotateRect(source, logical, page.rotation);
            element.style.left = (rect.x * 100 / output.width) + '%';
            heightScale = heightScale || 1;
            element.style.top = (rect.y * heightScale * 100 / output.height) + '%';
            element.style.width = (rect.w * 100 / output.width) + '%';
            element.style.height = (rect.h * heightScale * 100 / output.height) + '%';
        },

        _spRenderSurface(surface, page, editing, config) {
            if (!surface || !page) return;
            var model = this._spEnsureModel();
            config = config || model.config;
            this._spEnsurePageRects(page, config);
            this._spApplySurfaceTheme(surface, config, page);
            this._spApplySurfaceSize(surface, page, config);
            surface.classList.toggle('is-canvas-mode', editing && page.layoutMode === 'canvas');
            surface.innerHTML = '';
            var footerRatio = Math.min(0.3, 72 / rotatedSize(this._spReferenceSize(page, config), page.rotation).height);
            {
                var footer = document.createElement('nav'); footer.className = 'sp-device-footer';
                footer.style.height = (footerRatio * 100) + '%';
                var index = config.pages.indexOf(page);
                var ids = [config.settings.homePageId].concat(config.settings.bottomNavPageIds || []).concat(config.pages.map(function(item) { return item.id; }));
                var navPages = ids.filter(function(id, index) { return ids.indexOf(id) === index; }).map(function(id) { return config.pages.find(function(item) { return item.id === id; }); }).filter(Boolean);
                var more = config.pages.length > 5;
                navPages = navPages.slice(0, more ? 4 : 5);
                if (more) navPages.push({ id: '__more__', title: '更多' });
                footer.style.gridTemplateColumns = 'repeat(' + Math.max(1, navPages.length) + ', minmax(0, 1fr))';
                footer.innerHTML = navPages.map(function(item) {
                    var active = item.id === page.id;
                    return '<button type="button"' + (active ? ' class="is-active" aria-current="page"' : '') +
                        ' data-sp-editor-page="' + escapeHtml(item.id) + '">' + escapeHtml(item.title) + '</button>';
                }).join('');
                surface.appendChild(footer);
            }
            if (page.cards.length === 0) {
                var empty = document.createElement('div');
                empty.className = 'sp-empty-surface';
                empty.innerHTML = '<strong>空页面</strong><span>' + (editing ? '从内容库添加组件' : '尚未配置显示内容') + '</span>';
                surface.appendChild(empty);
                return;
            }
            page.cards.forEach((card) => {
                var element = document.createElement('article');
                element.className = 'sp-panel-card' + (editing && card.id === model.selectedCardId ? ' is-selected' : '');
                element.dataset.cardId = card.id;
                element.dataset.kind = card.kind;
                if (card.iconId) element.dataset.iconId = card.iconId;
                element.innerHTML = this._spCardContent(card, config);
                this._spApplyCardRect(element, card, page, config, 1 - footerRatio);
                if (editing) {
                    element.setAttribute('tabindex', '0');
                    element.setAttribute('aria-label', (card.title || CARD_LABELS[card.kind]) + '，' + CARD_LABELS[card.kind]);
                    if (page.layoutMode === 'canvas') {
                        var handle = document.createElement('button');
                        handle.type = 'button';
                        handle.className = 'sp-resize-handle';
                        handle.dataset.spResize = 'true';
                        handle.setAttribute('aria-label', '调整组件大小');
                        handle.title = '调整大小';
                        element.appendChild(handle);
                    }
                }
                surface.appendChild(element);
            });
        },

        _spRenderPageTabs() {
            var model = this._spEnsureModel();
            var container = screenElement('sp-page-tabs');
            if (!container || model.orderPointer) return;
            container.innerHTML = model.config.pages.map(function(page) {
                var active = page.id === model.selectedPageId;
                return '<button type="button" class="sp-page-tab' + (active ? ' is-active' : '') + '" data-page-id="' + escapeHtml(page.id) +
                    '" role="tab" aria-selected="' + active + '" title="拖动调整页面顺序"><span class="sp-sort-handle" aria-hidden="true">↕</span>' + escapeHtml(page.title) +
                    (page.id === model.config.settings.homePageId ? '<span class="sp-home-mark">首页</span>' : '') + '</button>';
            }).join('');
            var currentIndex = model.config.pages.findIndex(function(page) { return page.id === model.selectedPageId; });
            var moveUp = screenElement('sp-page-up');
            var moveDown = screenElement('sp-page-down');
            if (moveUp) moveUp.disabled = currentIndex <= 0;
            if (moveDown) moveDown.disabled = currentIndex < 0 || currentIndex >= model.config.pages.length - 1;
        },

        _spRenderPreview() {
            var model = this._spEnsureModel();
            var page = this._spCurrentPage();
            if (!page || model.pointer || model.orderPointer) return;
            this._spRenderSurface(screenElement('sp-editor-canvas'), page, true);
            this._spLoadVisibleHistories(page);
        },

        _spRenderSettings() {
            var model = this._spEnsureModel();
            var settings = model.config.settings;
            var setValue = function(id, value) { var el = screenElement(id); if (el) el.value = value; };
            setValue('sp-theme-id', settings.themeId || 'dark');
            setValue('sp-background-id', settings.background && settings.background.presetId || 'solid');
            setValue('sp-brightness', finiteNumber(settings.brightnessPercent, 70));
            var output = screenElement('sp-brightness-output');
            var continuous = model.capabilities.brightnessContinuous !== false;
            var brightness = screenElement('sp-brightness'); if (brightness) brightness.disabled = !continuous;
            if (output) output.textContent = continuous ? (Number(settings.brightnessPercent) === 0 ? '最低亮度' : finiteNumber(settings.brightnessPercent, 70) + '%') : this._spSystemActionReason('brightness');
            setValue('sp-confirm-idle', finiteNumber(settings.controlConfirmIdleSeconds, 60));
            var beep = screenElement('sp-touch-beep'); if (beep) { beep.checked = settings.touchBeepEnabled !== false; beep.disabled = this._spSystemActionSupport('buzzer') === false; }
            var beepInfo = screenElement('sp-beep-capability');
            if (beepInfo) beepInfo.textContent = this._spSystemActionSupport('buzzer') === false ? this._spSystemActionReason('buzzer') : '触摸时播放短提示音';
            var nav = screenElement('sp-bottom-nav');
            if (nav) nav.innerHTML = model.config.pages.map(function(page) { return '<option value="' + escapeHtml(page.id) + '"' + ((settings.bottomNavPageIds || []).indexOf(page.id) >= 0 ? ' selected' : '') + '>' + escapeHtml(page.title) + '</option>'; }).join('');
            var allowRestart = screenElement('sp-allow-restart');
            if (allowRestart) allowRestart.checked = settings.allowSystemRestart === true;
            this._spRenderSystemCapabilities();
            this._spChoiceOptions(screenElement('sp-display-ref'), model.displays.map(function(display) {
                var params = display.params || display.config || display;
                return { ref: String(display.id || display.ref || ''), label: choiceLabel(display) +
                    (params.width && params.height ? ' · ' + params.width + '×' + params.height : '') };
            }), model.config.displayRef, '未绑定屏幕');
            var page = this._spCurrentPage();
            var pageLimit = screenElement('sp-page-limit');
            if (pageLimit) pageLimit.textContent = model.config.pages.length + '/' + this._spLimits().maxPages + ' 页';
            setValue('sp-layout-template', page.layoutId || 'mixed-grid');
            document.querySelectorAll('[data-sp-layout-mode]').forEach(function(button) {
                button.classList.toggle('is-active', button.dataset.spLayoutMode === page.layoutMode);
            });
            this._spRenderHistorySettings();
        },

        _spSystemActionSupport(kind) {
            var capabilities = this._spEnsureModel().capabilities || {};
            var direct = { 'sleep-time': 'sleep', 'touch-beep': 'buzzer', 'confirm-idle': 'controlConfirmIdle', brightness: 'brightnessContinuous', sleep: 'sleep', buzzer: 'buzzer', restart: 'systemRestart' };
            if (Object.prototype.hasOwnProperty.call(capabilities, direct[kind])) return capabilities[direct[kind]] !== false;
            var actions = capabilities.systemActions || capabilities.displayActions || {};
            if (kind === 'sleep-time') kind = 'sleep';
            if (kind === 'touch-beep') kind = 'buzzer';
            if (Array.isArray(actions)) return actions.indexOf(kind) >= 0;
            return Object.prototype.hasOwnProperty.call(actions, kind) ? actions[kind] !== false : null;
        },

        _spSystemActionReason(kind) {
            var model = this._spEnsureModel();
            var caps = model.capabilities || {};
            var key = { 'sleep-time': 'sleep', 'touch-beep': 'buzzer', 'confirm-idle': 'controlConfirmIdle', brightness: 'brightnessContinuous', restart: 'systemRestart' }[kind] || kind;
            var reason = (caps.disabledReasons || {})[key];
            if (reason) return reason;
            if (caps.touchReady === false && caps.touchConfigured) return '触摸控制器未就绪，正在重试';
            if (caps.touchWake === false && model.application.runtime && model.application.runtime.running) return '触摸输入不可用，请检查显示外设的触摸配置';
            return '当前显示设备未提供“' + CARD_LABELS[kind] + '”功能';
        },

        _spRenderSystemCapabilities() {
            var model = this._spEnsureModel();
            var container = screenElement('sp-system-capabilities');
            var reasons = [];
            document.querySelectorAll('[data-sp-add]').forEach((button) => {
                var kind = button.dataset.spAdd;
                if (SYSTEM_ACTION_KINDS.indexOf(kind) < 0) return;
                var unavailable = this._spSystemActionSupport(kind) === false;
                button.disabled = unavailable;
                button.draggable = !unavailable;
                button.title = unavailable ? this._spSystemActionReason(kind) : CARD_LABELS[kind];
                if (unavailable && reasons.indexOf(button.title) < 0) reasons.push(button.title);
            });
            if (container) {
                container.textContent = reasons.join('；');
                container.classList.toggle('fb-hidden', !reasons.length);
            }
            [['sp-brightness', 'brightness'], ['sp-touch-beep', 'buzzer']].forEach(([id, kind]) => {
                var input = screenElement(id);
                if (!input) return;
                input.disabled = this._spSystemActionSupport(kind) === false;
                input.title = input.disabled ? this._spSystemActionReason(kind) : '';
            });
            var output = screenElement('sp-brightness-output');
            if (output) output.textContent = this._spSystemActionSupport('brightness') === false ? this._spSystemActionReason('brightness') :
                (Number(model.config.settings.brightnessPercent) === 0 ? '最低亮度' : finiteNumber(model.config.settings.brightnessPercent, 70) + '%');
            var beepInfo = screenElement('sp-beep-capability');
            if (beepInfo) beepInfo.textContent = this._spSystemActionSupport('buzzer') === false ? this._spSystemActionReason('buzzer') : '触摸时播放短提示音';
            this._spRenderAutoSleep();
        },

        _spRenderHistorySettings() {
            var model = this._spEnsureModel();
            var enabledInput = screenElement('sp-history-enabled');
            var sourceSelect = screenElement('sp-history-sources');
            var status = screenElement('sp-history-status');
            if (!enabledInput || !sourceSelect || !status) return;
            var capability = model.choiceCapabilities && model.choiceCapabilities.history || {};
            var supported = capability.supported !== false && !!model.historyConfig;
            var config = model.historyDraft || model.historyConfig || { enabled: false, sourceAllowlist: [] };
            var allowlist = config.sourceAllowlist || [];
            var choices = [{ ref: '__all__', label: '全部已开启记录的数值来源' }].concat(model.choices.filter(function(choice) {
                return !isControlChoice(choice) && String(choice.type || '').toLowerCase() === 'number';
            }));
            allowlist.forEach(function(ref) { if (!choices.some(function(item) { return choiceRef(item) === ref; })) choices.push({ ref: ref, label: ref + '（不可用，保留）', preserve: true }); });
            sourceSelect.innerHTML = choices.map(function(choice) { var ref = choiceRef(choice); return '<option value="' + escapeHtml(ref) + '"' +
                (allowlist.indexOf(ref) >= 0 || ref === '__all__' && !allowlist.length ? ' selected' : '') + (choice.preserve ? ' disabled data-preserve="true"' : '') + '>' + escapeHtml(choiceLabel(choice)) + '</option>'; }).join('');

            enabledInput.checked = config.enabled === true;
            enabledInput.disabled = !supported || config.writesPaused === true;
            sourceSelect.disabled = !supported || config.writesPaused === true;
            var persistence = model.historyStatus || {};
            status.textContent = !supported ? '不支持最近记录' : config.writesPaused === true ? '历史写入已暂停' :
                (config.enabled ? (persistence.persisted === false ? '已启用，配置待持久化' : '已启用，每个来源最多 10 条') : '已停用');
        },

        _spHistoryPayload() {
            var model = this._spEnsureModel();
            if (!model.historyConfig) return null;
            var draft = model.historyDraft || model.historyConfig;
            return { expectedRevision: model.historyConfig.revision, enabled: draft.enabled === true,
                sourceAllowlist: (draft.sourceAllowlist || []).slice() };
        },

        _spChangeHistoryDraft() {
            var model = this._spEnsureModel();
            if (!model.historyConfig) return;
            var selected = Array.from(screenElement('sp-history-sources').options).filter(function(option) {
                return option.selected || option.dataset.preserve === 'true';
            }).map(function(option) { return option.value; });
            model.historyDraft = Object.assign({}, model.historyConfig, { enabled: screenElement('sp-history-enabled').checked,
                sourceAllowlist: selected.indexOf('__all__') >= 0 ? [] : selected });
            model.historyDirty = JSON.stringify(model.historyDraft) !== JSON.stringify(model.historyConfig);
            this._spRenderDirtyState(); this._spPersistDraftSoon();
        },

        _spOpenProperties(mode) {
            var model = this._spEnsureModel();
            model.propertyMode = typeof mode === 'string' ? mode : mode === true ? 'global' : this._spCurrentCard() ? 'component' : 'page';
            model.globalProperties = model.propertyMode === 'global';
            var inspector = screenElement('sp-inspector'); if (inspector) inspector.classList.add('is-open');
            this._spRenderInspector();
        },

        _spRenderInspector() {
            var model = this._spEnsureModel();
            var card = this._spCurrentCard();
            var empty = screenElement('sp-inspector-empty');
            var form = screenElement('sp-inspector-form');
            if (!empty || !form) return;
            var mode = model.propertyMode || (model.globalProperties ? 'global' : card ? 'component' : 'page');
            var component = mode === 'component';
            var toggle = function(id, show) { var el = screenElement(id); if (el) el.classList.toggle('fb-hidden', !show); };
            toggle('sp-global-properties', mode === 'global');
            toggle('sp-component-inspector', component);
            toggle('sp-weather-properties', mode === 'weather' || component && card && card.kind === 'weather');
            toggle('sp-history-properties', mode === 'history' || component && card && card.kind === 'line');
            toggle('sp-page-properties', mode === 'page');
            [['sp-properties-toggle', 'page'], ['sp-component-properties', 'component'], ['sp-global-settings', 'global']].forEach(function(pair) {
                var button = screenElement(pair[0]); if (button) { button.setAttribute('aria-pressed', String(mode === pair[1])); button.classList.toggle('is-active', mode === pair[1]); }
            });
            var page = this._spCurrentPage();
            screenElement('sp-page-theme').value = page.themeId || '';
            screenElement('sp-page-background').value = page.background && page.background.presetId || '';
            empty.classList.toggle('fb-hidden', !!card);
            form.classList.toggle('fb-hidden', !card);
            if (!card) return;
            var kind = screenElement('sp-inspector-kind');
            var title = screenElement('sp-card-title');
            if (kind) kind.textContent = CARD_LABELS[card.kind] || card.kind;
            if (title) title.value = card.title || '';

            var numericControl = card.kind === 'control' && card.display === 'number';
            toggle('sp-source-field', card.kind !== 'weather' && SOURCE_KINDS.indexOf(card.kind) >= 0);
            toggle('sp-bar-sources-field', card.kind === 'bar');
            toggle('sp-control-field', card.kind === 'control' || card.kind === 'motor');
            toggle('sp-page-link-field', card.kind === 'page-link');
            toggle('sp-motor-fields', card.kind === 'motor');
            toggle('sp-numeric-fields', ['data', 'gauge', 'bar', 'line'].indexOf(card.kind) >= 0 || numericControl);
            toggle('sp-card-step-field', numericControl); toggle('sp-card-initial-field', numericControl);
            ['sp-card-min', 'sp-card-max'].forEach(function(id) { screenElement(id).closest('label').classList.toggle('fb-hidden', !numericControl && ['gauge', 'bar'].indexOf(card.kind) < 0); });
            toggle('sp-confirm-field', ['control', 'motor'].concat(SYSTEM_ACTION_KINDS).indexOf(card.kind) >= 0);
            [['speed','motorSpeed'], ['pulse','motorPulse'], ['speed-step','motorSpeedStep'], ['pulse-step','motorPulseStep']].forEach(function(pair) {
                var field = screenElement('sp-motor-' + pair[0]); if (field) field.value = card[pair[1]] === undefined ? CARD_DEFAULTS.motor[pair[1]] : card[pair[1]];
            });
            var dataChoices = model.choices.filter(function(item) {
                if (['bar', 'gauge', 'line'].indexOf(card.kind) >= 0) return helpers.isNumericChoice(item);
                return !isControlChoice(item) && !isWeatherChoice(item) && String(item.kind || '').toLowerCase() !== 'system';
            });
            this._spChoiceOptions(screenElement('sp-card-source'), dataChoices, card.sourceRef, '请选择数据来源');
            for (var seriesIndex = 1; seriesIndex < 4; seriesIndex++) {
                this._spChoiceOptions(screenElement('sp-card-source-' + seriesIndex), dataChoices,
                    card.sourceRefs && card.sourceRefs[seriesIndex] || '', '不添加对比来源');
            }
            this._spChoiceOptions(screenElement('sp-card-control'), model.choices.filter(function(choice) { return card.kind === 'motor' ? choice.type === 'motor' : isControlChoice(choice) && choice.type !== 'motor'; }), card.controlRef, '请选择控制项');

            this._spChoiceOptions(screenElement('sp-card-target-page'), model.config.pages, card.targetPageId, '选择目标页');

            var display = screenElement('sp-card-display');
            if (display) {
                var displayOptions = card.kind === 'control' ? [['switch', '开关'], ['action', '执行一次'], ['number', '数值调节']] : [[card.display || CARD_DEFAULTS[card.kind].display, CARD_LABELS[card.kind]]];
                display.innerHTML = displayOptions.map(function(option) { return '<option value="' + option[0] + '">' + option[1] + '</option>'; }).join('');
                display.value = card.display || '';
                display.disabled = card.kind === 'control' && model.choices.some(function(choice) { return choiceRef(choice) === card.controlRef; });
            }
            var values = {
                'sp-card-unit': card.unit || '', 'sp-card-precision': finiteNumber(card.precision, 1),
                'sp-card-min': finiteNumber(card.min, 0), 'sp-card-max': finiteNumber(card.max, 100),
                'sp-card-step': finiteNumber(card.step, 1), 'sp-card-initial': finiteNumber(card.initialValue, card.min || 0),
                'sp-card-size': card.size || CARD_DEFAULTS[card.kind].size
            };
            this._spRenderCardSize(card);
            ['x', 'y', 'w', 'h'].forEach(function(key) { values['sp-card-' + key] = card.rect && card.rect[key]; });
            Object.keys(values).forEach(function(key) { var input = screenElement(key); if (input) input.value = values[key]; });
            var confirm = screenElement('sp-card-confirm');
            if (confirm) confirm.checked = card.confirm === true;
            toggle('sp-display-field', card.kind === 'control');
        },

        _spRenderAll() {
            var model = this._spEnsureModel();
            if (!screenElement('screen-panel-page')) return;
            if (!model.config.pages.some(function(page) { return page.id === model.selectedPageId; })) model.selectedPageId = model.config.pages[0].id;
            var page = this._spCurrentPage();
            this._spEnsurePageRects(page);
            this._spRenderSettings();
            this._spRenderPageTabs();
            this._spMountInspector();
            this._spRenderPreview();
            var title = screenElement('sp-editor-title'); if (title) title.textContent = page.title;
            this._spRenderDirtyState();
            this._spApplyStatus();
            var validation = this._spValidateConfig(model.config);
            var summary = screenElement('sp-validation-summary');
            if (summary) summary.textContent = validation.errors.length ? (validation.errors.length + ' 项需要处理') : (model.config.pages.length + ' 页 · ' + validation.cardCount + ' 个组件');
        },

        _spValidateConfig(config) {
            return helpers.validateConfig(config, this);
        },

        _spSetupEvents() {
            var root = screenElement('screen-panel-page');
            if (!root || this._screenPanelEventsRoot === root) return;
            this._screenPanelEventsRoot = root;

            root.addEventListener('click', (event) => {
                if (Date.now() < (this._spEnsureModel().suppressClickUntil || 0) && event.target.closest('.sp-page-tab, .sp-panel-card')) { event.preventDefault(); return; }
                if (!event.target.closest('.sp-more summary')) root.querySelector('.sp-more')?.removeAttribute('open');
                var section = event.target.closest('[data-sp-section]');
                if (section) { root.querySelector('.sp-toolbox-content').scrollTop = screenElement(section.dataset.spSection).offsetTop; return; }
                var nav = event.target.closest('[data-sp-nav]');
                if (nav) { this._spNavigate(nav.dataset.spNav, nav.dataset.spTargetTab); return; }
                var pageTab = event.target.closest('.sp-page-tab[data-page-id]');
                if (pageTab) {
                    this._spEnsureModel().selectedPageId = pageTab.dataset.pageId;
                    this._spEnsureModel().selectedCardId = '';
                    this._spEnsureModel().propertyMode = 'page';
                    this._spRenderAll();
                    return;
                }
                var previewPage = event.target.closest('[data-sp-editor-page]');
                if (previewPage) {
                    var id = previewPage.dataset.spEditorPage;
                    if (id === '__more__') {
                        var surface = previewPage.closest('.sp-render-surface'), existing = surface.querySelector('.sp-nav-more');
                        if (existing) existing.remove(); else {
                            var menu = document.createElement('div'); menu.className = 'sp-nav-more';
                            menu.innerHTML = this._spEnsureModel().config.pages.map(function(page) {
                                return '<button type="button" data-sp-editor-page="' + escapeHtml(page.id) + '">' + escapeHtml(page.title) + '</button>';
                            }).join(''); surface.appendChild(menu);
                        }
                    } else { this._spEnsureModel().selectedPageId = id; this._spEnsureModel().selectedCardId = ''; this._spEnsureModel().propertyMode = 'page'; this._spRenderAll(); }
                    return;
                }
                var add = event.target.closest('[data-sp-add]');
                if (add) { this._spAddCard(add.dataset.spAdd); return; }
                var mode = event.target.closest('[data-sp-layout-mode]');
                if (mode) { this._spChangeLayoutMode(mode.dataset.spLayoutMode); return; }
                var cardElement = event.target.closest('#sp-editor-canvas .sp-panel-card[data-card-id]');
                if (cardElement && !event.target.closest('.sp-resize-handle')) {
                    if (Date.now() < (this._spEnsureModel().suppressClickUntil || 0)) return;
                    this._spEnsureModel().selectedCardId = cardElement.dataset.cardId;
                    this._spEnsureModel().globalProperties = false;
                    this._spRenderAll(); this._spOpenProperties(false);
                    return;
                }
                switch (event.target.closest('button')?.id || event.target.id) {
                    case 'sp-refresh-btn': this.loadScreenPanelPage({ noCache: true, preserveDraft: true }); break;
                    case 'sp-preview-refresh-btn': this._spRefreshState(true); break;
                    case 'sp-generate-three-screen': this._spGenerateThreeScreenTemplate(); break;
                    case 'sp-page-add': this._spAddPage(); break;
                    case 'sp-page-rename': this._spRenamePage(); break;
                    case 'sp-page-up': this._spMovePage(-1); break;
                    case 'sp-page-down': this._spMovePage(1); break;
                    case 'sp-page-home': this._spSetHomePage(); break;
                    case 'sp-page-delete': this._spDeletePage(); break;
                    case 'sp-apply-template': this._spApplyTemplate(); break;
                    case 'sp-undo': this._spUndo(); break;
                    case 'sp-redo': this._spRedo(); break;
                    case 'sp-card-delete': this._spDeleteCard(); break;
                    case 'sp-import-config': screenElement('sp-import-file')?.click(); break;
                    case 'sp-export-config': this._spExportConfig(); break;
                    case 'sp-reset-draft': this._spResetDraft(); break;
                    case 'sp-draft-restore': {
                        this._spRecoverDraft();
                        screenElement('sp-draft-recovery').classList.add('fb-hidden'); break;
                    }
                    case 'sp-draft-discard': this._spClearDraft(); screenElement('sp-draft-recovery').classList.add('fb-hidden'); break;
                    case 'sp-global-settings': this._spOpenProperties('global'); break;
                    case 'sp-properties-toggle': this._spOpenProperties('page'); break;
                    case 'sp-component-properties': this._spOpenProperties('component'); break;
                    case 'sp-history-settings': this._spOpenProperties('history'); break;
                    case 'sp-weather-settings': this._spOpenProperties('weather'); break;
                    case 'sp-properties-close': screenElement('sp-inspector').classList.remove('is-open'); screenElement('sp-properties-toggle').focus(); break;
                    case 'sp-save-config': this._spSaveAll(); break;
                    case 'sp-weather-refresh': this._spRefreshWeather(); break;
                }
            });

            root.addEventListener('change', (event) => {
                if (event.target.id === 'sp-auto-sleep-enabled') { this._spSetAutoSleepEnabled(event.target.checked); return; }
                if (event.target.id === 'sp-idle-sleep' && Number(event.target.value) > 0 && this._spSystemActionSupport('sleep-time') === false) { this._spRenderAutoSleep(); return; }
                if (event.target.id.startsWith('sp-history-')) { this._spChangeHistoryDraft(); return; }
                if (event.target.id === 'sp-bottom-nav') {
                    this._spMutate(function(config) { config.settings.bottomNavPageIds = Array.from(event.target.selectedOptions).map(function(option) { return option.value; }).slice(0, 5); }); return;
                }
                if (event.target.id === 'sp-page-theme' || event.target.id === 'sp-page-background') {
                    this._spMutate(() => { var page = this._spCurrentPage(); if (event.target.id === 'sp-page-theme') { if (event.target.value) page.themeId = event.target.value; else delete page.themeId; }
                        else { if (event.target.value) page.background = { presetId: event.target.value }; else delete page.background; } }); return;
                }
                if (event.target.closest('#sp-inspector-form')) { this._spApplyInspector(true, event.target); if (event.target.tagName === 'SELECT') this._spRenderInspector(); return; }
                if (event.target.id.startsWith('sp-weather-')) {
                    if (event.target.id === 'sp-weather-clear-key' && event.target.checked) screenElement('sp-weather-enabled').checked = false;
                    this._spEnsureModel().weatherDirty = true;
                    this._spRenderDirtyState(); return;
                }
                if (event.target.id === 'sp-layout-template') {
                    this._spMutate(() => { var page = this._spCurrentPage(); page.layoutId = event.target.value; this._spLayoutTemplatePage(page); });
                    return;
                }
                var settingMap = {
                    'sp-display-ref': 'displayRef', 'sp-theme-id': 'themeId',
                    'sp-idle-sleep': 'idleSleepSeconds', 'sp-allow-restart': 'allowSystemRestart',
                    'sp-touch-beep': 'touchBeepEnabled', 'sp-confirm-idle': 'controlConfirmIdleSeconds'
                };
                if (event.target.id === 'sp-background-id') {
                    this._spMutate(function(config) { config.settings.background.presetId = event.target.value; });
                } else if (settingMap[event.target.id]) {
                    var key = settingMap[event.target.id];
                    var value = event.target.type === 'checkbox' ? event.target.checked : event.target.value;
                    if (key === 'idleSleepSeconds' || key === 'controlConfirmIdleSeconds') value = Number(value);
                    this._spMutate((config) => {
                        if (key === 'displayRef') {
                            config.displayRef = value || null;
                            var selectedDisplay = this._spEnsureModel().displays.find(function(display) {
                                return String(display.id || display.ref || '') === String(config.displayRef || '');
                            });
                            var params = selectedDisplay && (selectedDisplay.params || selectedDisplay.config || selectedDisplay);
                            if (params && Number(params.width) > 0 && Number(params.height) > 0) {
                                var rotation = Number(params.rotation) || 0;
                                var degrees = rotation >= 0 && rotation <= 3 ? rotation * 90 : rotation;
                                var width = Number(params.width);
                                var height = Number(params.height);
                                config.pages.forEach(function(page) {
                                    page.canvas = { width: width, height: height };
                                    page.rotation = [0, 90, 180, 270].indexOf(degrees) >= 0 ? degrees : 0;
                                });
                            }
                        }
                        else config.settings[key] = value;
                        if (key === 'displayRef') config.pages.forEach((page) => { this._spLayoutTemplatePage(page); });
                    });
                }
            });

            root.addEventListener('input', (event) => {
                if (event.target.id.startsWith('sp-weather-')) { this._spEnsureModel().weatherDirty = true; this._spRenderDirtyState(); }
                if (event.target.closest('#sp-inspector-form') && event.target.tagName === 'INPUT') this._spApplyInspector(true, event.target);
                if (event.target.id === 'sp-brightness') {
                    this._spEnsureModel().config.settings.brightnessPercent = finiteNumber(event.target.value, 70);
                    this._spMarkDirty({ render: false });
                }
                var outputMap = { 'sp-brightness': 'sp-brightness-output' };
                var output = screenElement(outputMap[event.target.id]);
                if (output) output.textContent = Number(event.target.value) === 0 ? '最低亮度' : event.target.value + '%';
            });
            root.addEventListener('focusin', (event) => {
                if (event.target.closest('#sp-inspector-form')) this._spEnsureModel().inspectorEdit = { element: event.target, saved: false };
            });
            var brightness = screenElement('sp-brightness');
            if (brightness) brightness.addEventListener('change', (event) => {
                this._spMutate(function(config) { config.settings.brightnessPercent = finiteNumber(event.target.value, 70); });
            });
            root.addEventListener('submit', (event) => {
                if (event.target.id === 'sp-inspector-form') { event.preventDefault(); this._spApplyInspector(); }
            });
            var importFile = screenElement('sp-import-file');
            if (importFile) importFile.addEventListener('change', (event) => this._spImportConfig(event.target.files && event.target.files[0]));
            root.addEventListener('dragstart', (event) => {
                var button = event.target.closest('[data-sp-add]');
                if (button && event.dataTransfer) { event.dataTransfer.setData('application/x-fastbee-component', button.dataset.spAdd); event.dataTransfer.effectAllowed = 'copy'; }
            });
            root.addEventListener('pointerdown', (event) => {
                var pageTab = event.target.closest('.sp-page-tab[data-page-id]');
                if (pageTab) this._spOrderPointerDown(event, 'page', pageTab);
            });
            var canvas = screenElement('sp-editor-canvas');
            if (canvas) {
                canvas.addEventListener('dragover', function(event) {
                    if (Array.from(event.dataTransfer.types).indexOf('application/x-fastbee-component') >= 0) { event.preventDefault(); event.dataTransfer.dropEffect = 'copy'; }
                });
                canvas.addEventListener('drop', (event) => {
                    var kind = event.dataTransfer.getData('application/x-fastbee-component'); if (!kind) return;
                    event.preventDefault(); this._spDropComponent(kind, event.clientX, event.clientY);
                });
                canvas.addEventListener('pointerdown', (event) => {
                    var card = event.target.closest('.sp-panel-card[data-card-id]');
                    if (card && this._spCurrentPage().layoutMode === 'template') this._spOrderPointerDown(event, 'card', card);
                    else this._spPointerDown(event);
                });
                canvas.addEventListener('keydown', (event) => this._spCanvasKeydown(event));
            }
            root.addEventListener('keydown', (event) => {
                if (event.key === 'Escape' && this._spEnsureModel().orderPointer) { event.preventDefault(); this._spOrderPointerUp({ type: 'pointercancel' }); return; }
                if (event.key === 'Escape') { screenElement('sp-inspector')?.classList.remove('is-open'); root.querySelector('.sp-more')?.removeAttribute('open'); }
                if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'z') {
                    if (event.target.matches('input, textarea, select')) return;
                    event.preventDefault();
                    if (event.shiftKey) this._spRedo(); else this._spUndo();
                }
            });
        },

        _spNavigate(target, targetTab) {
            var pageMap = { features: 'features', peripheral: 'peripheral', 'periph-exec': 'periph-exec', protocol: 'protocol' };
            var page = pageMap[target];
            if (!page) return;
            this._spPersistDraftSoon();
            Promise.resolve(this.changePage(page)).then(() => {
                if (targetTab && page === 'protocol') this.showConfigTab('protocol-page', targetTab);
            });
        },

        _spSupportedTemplate(page) {
            page.cards = page.cards.filter((card) => SYSTEM_ACTION_KINDS.indexOf(card.kind) < 0 || this._spSystemActionSupport(card.kind) !== false);
            return page;
        },

        async _spGenerateThreeScreenTemplate() {
            var model = this._spEnsureModel();
            var limits = this._spLimits();
            if (limits.maxPages < 4 || limits.maxCards < 5) {
                this._spSetMessage('资源不足，请单独添加页面', 'error');
                return;
            }
            var hasContent = model.config.pages.length > 1 || model.config.pages.some(function(page) { return page.cards.length > 0; });
            if (hasContent && !await FBConfirm({ message: '替换当前草稿？保存后生效。' })) return;

            var current = this._spCurrentPage();
            var size = this._spReferenceSize(current);
            var rotation = current && current.rotation || 0;
            var config = helpers.buildThreeScreenTemplate(model.config, model.choices, size, rotation);
            config.pages.forEach((page) => this._spLayoutTemplatePage(this._spSupportedTemplate(page), config));
            this._spPushUndo();
            model.config = config;
            model.selectedPageId = 'home';
            model.selectedCardId = '';
            this._spMarkDirty();
            this._spSetMessage('起始界面已生成', 'success');
        },

        async _spAddPage() {
            var model = this._spEnsureModel();
            var limits = this._spLimits();
            if (model.config.pages.length >= limits.maxPages) { this._spSetMessage('页面数量已达上限', 'error'); return; }
            var preset = screenElement('sp-page-preset');
            var template = preset && preset.value || 'blank';
            var id = safeId('page');
            var currentPage = this._spCurrentPage();
            var newPage = helpers.buildPageTemplate(template, model.config, model.choices,
                this._spReferenceSize(currentPage), currentPage.rotation || 0, id);
            this._spSupportedTemplate(newPage);
            var existingCards = model.config.pages.reduce(function(total, page) { return total + page.cards.length; }, 0);
            if (newPage.cards.length > limits.maxCardsPerPage || existingCards + newPage.cards.length > limits.maxCards) {
                this._spSetMessage('组件超限，请删除组件或添加空白页', 'error');
                return;
            }
            var title = await FBConfirm({ input: { label: '页面名称', value: newPage.title } });
            if (title === null) return;
            newPage.title = title.trim().slice(0, 64) || newPage.title;
            this._spLayoutTemplatePage(newPage, model.config);
            this._spMutate(function(config) {
                config.pages.push(newPage);
                model.selectedPageId = id;
                model.selectedCardId = '';
            });
        },

        async _spRenamePage() {
            var page = this._spCurrentPage();
            var title = await FBConfirm({ input: { label: '页面名称', value: page.title } });
            if (title === null) return;
            title = title.trim().slice(0, 64);
            if (!title || title === page.title) return;
            this._spMutate(function() { page.title = title; });
        },

        _spOrderPointerDown(event, kind, element) {
            var model = this._spEnsureModel();
            if (event.button !== 0 || event.isPrimary === false || model.pointer || model.orderPointer || !model.loaded) return;
            if (kind === 'page' && event.pointerType === 'touch' && !event.target.closest('.sp-sort-handle')) return;
            event.preventDefault();
            var pointer = model.orderPointer = { kind: kind, pointerId: event.pointerId,
                sourceId: kind === 'page' ? element.dataset.pageId : element.dataset.cardId,
                pageId: model.selectedPageId, config: model.config,
                startX: event.clientX, startY: event.clientY, moved: false, targetId: '', element: element };
            var move = (e) => this._spOrderPointerMove(e);
            var up = (e) => this._spOrderPointerUp(e);
            var cancel = () => this._spOrderPointerUp({ type: 'pointercancel' });
            pointer.cleanup = function() {
                document.removeEventListener('pointermove', move); document.removeEventListener('pointerup', up);
                document.removeEventListener('pointercancel', up); window.removeEventListener('blur', cancel);
            };
            document.addEventListener('pointermove', move); document.addEventListener('pointerup', up);
            document.addEventListener('pointercancel', up); window.addEventListener('blur', cancel);
        },

        _spOrderPointerMove(event) {
            var pointer = this._spEnsureModel().orderPointer;
            if (!pointer || event.pointerId !== pointer.pointerId) return;
            if (!pointer.moved && Math.hypot(event.clientX - pointer.startX, event.clientY - pointer.startY) < 6) return;
            pointer.moved = true;
            pointer.element.classList.add('is-sorting');
            var container = screenElement(pointer.kind === 'page' ? 'sp-page-tabs' : 'sp-editor-canvas');
            var hit = document.elementFromPoint(event.clientX, event.clientY);
            var target = hit && hit.closest(pointer.kind === 'page' ? '.sp-page-tab[data-page-id]' : '.sp-panel-card[data-card-id]');
            pointer.targetId = target && container?.contains(target) ? (pointer.kind === 'page' ? target.dataset.pageId : target.dataset.cardId) : '';
            if (pointer.targetId === pointer.sourceId) pointer.targetId = '';
            container?.querySelectorAll('.is-sort-target').forEach(function(item) { item.classList.remove('is-sort-target'); });
            if (pointer.targetId) target.classList.add('is-sort-target');
        },

        _spOrderPointerUp(event) {
            var model = this._spEnsureModel(), pointer = model.orderPointer;
            if (!pointer || (event.pointerId !== undefined && event.pointerId !== pointer.pointerId)) return;
            if (event.type === 'pointerup') this._spOrderPointerMove(event);
            pointer.cleanup(); model.orderPointer = null;
            pointer.element.classList.remove('is-sorting');
            screenElement('screen-panel-page')?.querySelectorAll('.is-sort-target').forEach(function(item) { item.classList.remove('is-sort-target'); });
            if (!pointer.moved) return;
            model.suppressClickUntil = Date.now() + 250;
            if (event.type === 'pointerup' && pointer.config === model.config && pointer.pageId === model.selectedPageId && pointer.targetId) {
                this._spReorderItem(pointer.kind, pointer.sourceId, pointer.targetId);
            }
        },

        async _spDeletePage() {
            var model = this._spEnsureModel();
            var page = this._spCurrentPage();
            if (model.config.pages.length <= 1) {
                this._spSetMessage('至少保留一个页面', 'error');
                return;
            }
            if (!await FBConfirm({ message: '删除“' + page.title + '”？其中组件会一并移除。', danger: true })) return;
            this._spMutate(function(config) {
                config.pages = config.pages.filter(function(item) { return item.id !== page.id; });
                config.pages.forEach(function(item) {
                    item.cards.forEach(function(card) { if (card.targetPageId === page.id) card.targetPageId = ''; });
                });
                if (config.settings.homePageId === page.id) config.settings.homePageId = config.pages[0].id;
                model.selectedPageId = config.pages[0].id;
                model.selectedCardId = '';
            });
        },

        _spAddCard(kind, dropped) {
            var executeOnce = kind === 'execute';
            if (executeOnce) kind = 'control';
            var model = this._spEnsureModel();
            var page = this._spCurrentPage();
            var limits = this._spLimits();
            var total = model.config.pages.reduce(function(sum, item) { return sum + item.cards.length; }, 0);
            if (total >= limits.maxCards || page.cards.length >= limits.maxCardsPerPage) {
                this._spSetMessage('当前容量已满，请删除组件或添加页面', 'error');
                return;
            }
            if (!CARD_DEFAULTS[kind]) return;
            if (kind === 'motor' && !model.choices.some(function(choice) { return choice.type === 'motor'; })) { this._spSetMessage('请先配置电机', 'error'); return; }
            if (kind === 'restart' && model.config.settings.allowSystemRestart !== true) {
                this._spOpenProperties('global');
                this._spSetMessage('请先在全局属性中开启“允许从屏幕重启设备”', 'error');
                return;
            }
            var systemSupport = SYSTEM_ACTION_KINDS.indexOf(kind) >= 0 ? this._spSystemActionSupport(kind) : null;
            if (systemSupport === false) {
                this._spSetMessage(this._spSystemActionReason(kind), 'error');
                return;
            }
            var historyRequired = kind === 'line' && (!model.historyConfig || model.historyConfig.enabled !== true);
            var card = Object.assign({
                id: safeId('card'), kind: kind, sourceRef: '', controlRef: '', targetPageId: '',
                unit: '', precision: 1, min: 0, max: 100, step: 1, initialValue: 0,
                confirm: false
            }, clone(CARD_DEFAULTS[kind]));
            if (kind === 'weather') {
                var source = model.choices.find(isWeatherChoice); card.sourceRef = source ? choiceRef(source) : '';
                var enabled = screenElement('sp-weather-enabled');
                if (enabled && !enabled.checked) { enabled.checked = true; model.weatherDirty = true; }
            }
            if (kind === 'motor') card.controlRef = choiceRef(model.choices.find(function(choice) { return choice.type === 'motor'; }));
            if (executeOnce) { card.title = '执行一次'; card.display = 'action'; }
            if (kind === 'brightness') card.actionValue = clamp(Math.round(finiteNumber(model.config.settings.brightnessPercent, 70)), 0, 100);
            if (kind === 'page-link') card.targetPageId = model.config.pages.find(function(item) { return item.id !== page.id; })?.id || page.id;
            if (page.layoutMode === 'canvas' || dropped) {
                card.rect = this._spPresetRect(card, page);
                if (!this._spPlaceCard(page, card)) { this._spSetLayoutError('没有足够空间，请缩小组件或新增页面'); return; }
            }
            this._spMutate(() => {
                if (dropped) page.layoutMode = 'canvas';
                page.cards.push(card);
                if (page.layoutMode === 'template') this._spLayoutTemplatePage(page);
                model.selectedCardId = card.id; model.globalProperties = false; model.propertyMode = 'component';
            });
            this._spOpenProperties(false);
            this._spSetMessage(historyRequired ?
                '保存时启用所选来源记录。' :
                ('已添加“' + card.title + '”，请设置属性。'), historyRequired ? 'error' : 'success');
        },

        _spDeleteCard() {
            var model = this._spEnsureModel();
            var page = this._spCurrentPage();
            var card = this._spCurrentCard();
            if (!card) return;
            this._spMutate(() => {
                page.cards = page.cards.filter(function(item) { return item.id !== card.id; });
                if (page.layoutMode === 'template') this._spLayoutTemplatePage(page);
                model.selectedCardId = '';
            });
        },

        _spPresetRect(card, page) {
            var size = this._spReferenceSize(page), min = this._spMinimumSize(card);
            var w = card.size === 'small' ? Math.floor((size.width - 32) / 2) : size.width - 24;
            var h = card.size === 'large' ? Math.floor(size.height * .55) : card.size === 'wide' ? 144 : 104;
            return { x: card.rect ? card.rect.x : 8, y: card.rect ? card.rect.y : 8, w: Math.min(size.width, Math.max(min.w, w)), h: Math.min(size.height, Math.max(min.h, h)) };
        },

        _spMinimumSize(card) {
            return helpers.minimumCardSize(card);
        },

        _spRenderCardSize(card) {
            var select = screenElement('sp-card-size');
            if (!select) return;
            select.innerHTML = '<option value="small">小</option><option value="wide">宽</option><option value="large">大</option>' +
                '<option value="custom" disabled>自定义' + (card.rect ? '（' + Number(card.rect.w) + ' × ' + Number(card.rect.h) + '）' : '') + '</option>';
            select.value = card.size || CARD_DEFAULTS[card.kind].size;
        },

        _spPlaceCard(page, card) {
            var size = this._spReferenceSize(page);
            for (var y = 8; y + card.rect.h <= size.height; y += 8) {
                for (var x = 8; x + card.rect.w <= size.width; x += 8) {
                    card.rect.x = x; card.rect.y = y;
                    if (!this._spRectCollision(page, card)) return true;
                }
            }
            return false;
        },

        _spApplyInspector(live, input) {
            var card = this._spCurrentCard();
            var page = this._spCurrentPage();
            if (!card) return;
            if (input && input.type === 'number' && (input.value === '' || !input.validity.valid)) return;
            var model = this._spEnsureModel(), before = clone(model.config), oldSize = card.size;
            var get = function(id) { return screenElement(id); };
            var barRefs = [];
            if (card.kind === 'bar') {
                barRefs = [get('sp-card-source')?.value || '', get('sp-card-source-1')?.value || '',
                    get('sp-card-source-2')?.value || '', get('sp-card-source-3')?.value || ''].filter(Boolean);
                var selectedChoices = barRefs.map((ref) => this._spEnsureModel().choices.find(function(choice) { return choiceRef(choice) === ref; }));
                if ((barRefs.length && !get('sp-card-source')?.value) || new Set(barRefs).size !== barRefs.length ||
                    selectedChoices.some(function(choice) { return choice && !helpers.isNumericChoice(choice); }) ||
                    selectedChoices.some(function(choice) { return choice && selectedChoices[0] && String(choice.unit || '') !== String(selectedChoices[0].unit || ''); })) {
                    this._spSetMessage('请选择不重复的同单位数值来源', 'error');
                    return;
                }
            }
            {
                card.title = String(get('sp-card-title')?.value || CARD_LABELS[card.kind]).trim().slice(0, 64);
                if (card.kind !== 'weather') card.sourceRef = get('sp-card-source')?.value || '';
                if (card.kind === 'bar') card.sourceRefs = barRefs.length > 1 ? barRefs : [];
                card.controlRef = get('sp-card-control')?.value || '';
                card.targetPageId = get('sp-card-target-page')?.value || '';
                card.display = get('sp-card-display')?.value || card.display;
                if (card.kind === 'control') {
                    var selectedControl = this._spEnsureModel().choices.find(function(choice) {
                        return choiceRef(choice) === card.controlRef;
                    });
                    var controlType = String(selectedControl && selectedControl.type || '').toLowerCase();
                    if (controlType === 'switch' || controlType === 'action' || controlType === 'number') card.display = controlType;
                }
                if (card.kind === 'motor') [['speed','motorSpeed'], ['pulse','motorPulse'], ['speed-step','motorSpeedStep'], ['pulse-step','motorPulseStep']].forEach(function(pair) { card[pair[1]] = Number(get('sp-motor-' + pair[0]).value); });
                card.unit = String(get('sp-card-unit')?.value || '').trim().slice(0, 16);
                card.precision = clamp(finiteNumber(get('sp-card-precision')?.value, 1), 0, 6);
                card.min = finiteNumber(get('sp-card-min')?.value, 0);
                card.max = finiteNumber(get('sp-card-max')?.value, 100);
                card.step = finiteNumber(get('sp-card-step')?.value, 1);
                card.initialValue = finiteNumber(get('sp-card-initial')?.value, card.min);
                card.size = get('sp-card-size')?.value || card.size || CARD_DEFAULTS[card.kind].size;
                if (card.kind === 'control' && card.display === 'number' && card.size === 'small') card.size = 'large';
                card.confirm = get('sp-card-confirm')?.checked === true;
                var geometry = input && ['sp-card-x', 'sp-card-y', 'sp-card-w', 'sp-card-h'].indexOf(input.id) >= 0;
                if (geometry || card.size !== oldSize) {
                    var rect = geometry ? clone(card.rect) : this._spPresetRect(card, page);
                    if (geometry) rect[input.id.slice(-1)] = Number(input.value);
                    var size = this._spReferenceSize(page), minimum = this._spMinimumSize(card);
                    if (!geometry) { rect.x = clamp(rect.x, 0, size.width - rect.w); rect.y = clamp(rect.y, 0, size.height - rect.h); }
                    if (rect.w < minimum.w || rect.h < minimum.h || rect.x < 0 || rect.y < 0 || rect.x + rect.w > size.width || rect.y + rect.h > size.height) {
                        model.config = before;
                        this._spSetLayoutError('尺寸至少 ' + minimum.w + ' × ' + minimum.h + '，位置和尺寸不能超出画布'); return;
                    }
                    card.rect = rect; page.layoutMode = 'canvas';
                    if (geometry) { card.size = 'custom'; this._spRenderCardSize(card); }
                }
            }
            if (JSON.stringify(before) === JSON.stringify(model.config)) return;
            var edit = model.inspectorEdit;
            if (!input || input.tagName === 'SELECT' || !edit || edit.element !== input || !edit.saved) {
                model.undo.push(before); if (model.undo.length > HISTORY_LIMIT) model.undo.shift(); model.redo = [];
                if (edit && edit.element === input) edit.saved = true;
            }
            this._spSetLayoutError(this._spRectCollision(page, card) ? '组件重叠，请调整位置或尺寸后保存' : '');
            this._spMarkDirty({ render: !live });
            if (live) {
                this._spRenderSurface(screenElement('sp-editor-canvas'), page, true);
                document.querySelectorAll('[data-sp-layout-mode]').forEach(function(button) { button.classList.toggle('is-active', button.dataset.spLayoutMode === page.layoutMode); });
                this._spLoadVisibleHistories(page);
                    var validation = this._spValidateConfig(this._spEnsureModel().config);
                screenElement('sp-validation-summary').textContent = validation.errors.length ? validation.errors.length + ' 项需要处理' : '修改已加入草稿';
            }
        },

        _spChangeLayoutMode(mode) {
            var page = this._spCurrentPage();
            if (mode !== 'template' && mode !== 'canvas') return;
            if (page.layoutMode === mode) return;
            this._spMutate(() => {
                page.layoutMode = mode;
                if (mode === 'template') this._spLayoutTemplatePage(page);
            });
        },

        async _spApplyTemplate() {
            var page = this._spCurrentPage();
            if (page.cards.length > 0 && !await FBConfirm({ message: '按模板重新排版？绑定保留。' })) return;
            this._spMutate(() => { page.layoutMode = 'template'; this._spLayoutTemplatePage(page); });
        },

        _spUndo() { this._spTravelHistory(false); },
        _spRedo() { this._spTravelHistory(true); },
        _spTravelHistory(forward) {
            var model = this._spEnsureModel();
            var source = forward ? model.redo : model.undo, target = forward ? model.undo : model.redo;
            if (!source.length) return;
            target.push(clone(model.config));
            model.config = normalizeConfig(source.pop());
            if (!model.config.pages.some(function(page) { return page.id === model.selectedPageId; })) model.selectedPageId = model.config.pages[0].id;
            model.selectedCardId = '';
            this._spMarkDirty();
        },

        async _spResetDraft() {
            var model = this._spEnsureModel();
            if ((model.dirty || model.weatherDirty || model.historyDirty) && !await FBConfirm({ message: '撤销未保存修改？', danger: true })) return;
            if (model.configConflict) {
                model.revision = model.configConflict.revision;
                model.savedConfig = normalizeConfig(model.configConflict.config);
                model.configConflict = null;
            }
            model.weatherDirty = false; model.historyDirty = false; model.historyDraft = clone(model.historyConfig); this._spRenderWeather();
            model.config = normalizeConfig(model.savedConfig);
            model.selectedPageId = model.config.settings.homePageId || model.config.pages[0].id;
            model.selectedCardId = '';
            model.undo = [];
            model.redo = [];
            model.dirty = false;
            this._spClearDraft();
            this._spSetMessage('已恢复设备保存的配置', 'success');
            this._spRenderAll();
        },

        _spCanvasKeydown(event) {
            var cardElement = event.target.closest('.sp-panel-card[data-card-id]');
            if (!cardElement || ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].indexOf(event.key) < 0) return;
            var model = this._spEnsureModel();
            var page = this._spCurrentPage();
            if (page.layoutMode !== 'canvas') return;
            var card = page.cards.find(function(item) { return item.id === cardElement.dataset.cardId; });
            if (!card) return;
            event.preventDefault();
            var size = this._spReferenceSize(page);
            var output = rotatedSize(size, page.rotation);
            var displayRect = rotateRect(card.rect, size, page.rotation);
            var step = event.shiftKey ? 16 : 8;
            this._spPushUndo();
            if (event.key === 'ArrowLeft') displayRect.x = clamp(displayRect.x - step, 0, output.width - displayRect.w);
            if (event.key === 'ArrowRight') displayRect.x = clamp(displayRect.x + step, 0, output.width - displayRect.w);
            if (event.key === 'ArrowUp') displayRect.y = clamp(displayRect.y - step, 0, output.height - displayRect.h);
            if (event.key === 'ArrowDown') displayRect.y = clamp(displayRect.y + step, 0, output.height - displayRect.h);
            var logicalRect = unrotateRect(displayRect, size, page.rotation);
            card.rect = {
                x: Math.round(logicalRect.x), y: Math.round(logicalRect.y),
                w: Math.round(logicalRect.w), h: Math.round(logicalRect.h)
            };
            if (this._spRectCollision(page, card)) {
                model.config = normalizeConfig(model.undo.pop());
                this._spSetLayoutError('组件重叠，已恢复原位置');
            } else {
                this._spSetLayoutError('');
            }
            this._spMarkDirty();
            var moved = screenElement('sp-editor-canvas')?.querySelector('[data-card-id="' + card.id + '"]');
            moved?.focus({ preventScroll: true });
        },

        _spDropComponent(kind, clientX, clientY) {
            var model = this._spEnsureModel(), page = this._spCurrentPage(), count = page.cards.length;
            this._spAddCard(kind, true);
            if (page.cards.length === count) return;
            var card = this._spCurrentCard(), canvas = screenElement('sp-editor-canvas');
            var bounds = canvas.getBoundingClientRect(), size = this._spReferenceSize(page), output = rotatedSize(size, page.rotation);
            var rect = rotateRect(card.rect, size, page.rotation), before = clone(card.rect);
            rect.x = clamp(snap((clientX - bounds.left) / Math.max(1, bounds.width) * output.width - rect.w / 2, 8), 0, output.width - rect.w);
            rect.y = clamp(snap((clientY - bounds.top) / Math.max(1, bounds.height * (1 - Math.min(0.3, 72 / output.height))) * output.height - rect.h / 2, 8), 0, output.height - rect.h);
            card.rect = unrotateRect(rect, size, page.rotation);
            if (this._spRectCollision(page, card)) { card.rect = before; this._spSetLayoutError('组件重叠，已添加到可用位置'); }
            else { page.layoutMode = 'canvas'; this._spSetLayoutError(''); }
            this._spMarkDirty();
        },

        _spPointerDown(event) {
            var element = event.target.closest('.sp-panel-card[data-card-id]');
            var page = this._spCurrentPage();
            if (!element || !page || page.layoutMode !== 'canvas' || event.button !== 0) return;
            var card = page.cards.find(function(item) { return item.id === element.dataset.cardId; });
            if (!card || !card.rect) return;
            event.preventDefault();
            var model = this._spEnsureModel();
            if (model.pointer || event.isPrimary === false) return;
            var logicalSize = this._spReferenceSize(page);
            model.selectedCardId = card.id;
            model.propertyMode = 'component';
            screenElement('sp-editor-canvas').querySelectorAll('.sp-panel-card').forEach(function(item) { item.classList.toggle('is-selected', item === element); });
            model.pointer = {
                pointerId: event.pointerId,
                mode: event.target.closest('[data-sp-resize]') ? 'resize' : 'move',
                cardId: card.id,
                startX: event.clientX,
                startY: event.clientY,
                startRect: clone(card.rect),
                startDisplayRect: rotateRect(card.rect, logicalSize, page.rotation),
                snapshot: clone(model.config),
                moved: false
            };
            element.setPointerCapture?.(event.pointerId);
            var canvas = screenElement('sp-editor-canvas');
            canvas?.classList.add('is-dragging');
            var move = (moveEvent) => this._spPointerMove(moveEvent);
            var up = (upEvent) => {
                if (upEvent.pointerId !== event.pointerId) return;
                document.removeEventListener('pointermove', move);
                document.removeEventListener('pointerup', up);
                document.removeEventListener('pointercancel', up);
                this._spPointerUp(upEvent);
            };
            document.addEventListener('pointermove', move);
            document.addEventListener('pointerup', up);
            document.addEventListener('pointercancel', up);
        },

        _spPointerMove(event) {
            var model = this._spEnsureModel();
            var pointer = model.pointer;
            if (!pointer || event.pointerId !== pointer.pointerId) return;
            var page = this._spCurrentPage();
            var card = page.cards.find(function(item) { return item.id === pointer.cardId; });
            var canvas = screenElement('sp-editor-canvas');
            var element = canvas && Array.prototype.find.call(canvas.querySelectorAll('.sp-panel-card[data-card-id]'), function(candidate) {
                return candidate.dataset.cardId === pointer.cardId;
            });
            if (!card || !canvas || !element) return;
            var bounds = canvas.getBoundingClientRect();
            var size = this._spReferenceSize(page);
            var output = rotatedSize(size, page.rotation);
            var dx = (event.clientX - pointer.startX) * output.width / Math.max(bounds.width, 1);
            var dy = (event.clientY - pointer.startY) * output.height / Math.max(bounds.height * (1 - Math.min(0.3, 72 / output.height)), 1);
            if (Math.abs(dx) + Math.abs(dy) < 2) return;
            pointer.moved = true;
            var displayRect = clone(pointer.startDisplayRect || rotateRect(pointer.startRect, size, page.rotation));
            if (pointer.mode === 'resize') {
                var minimum = this._spMinimumSize(card), rotated = page.rotation === 90 || page.rotation === 270;
                displayRect.w = clamp(snap(displayRect.w + dx, 8), rotated ? minimum.h : minimum.w, output.width - displayRect.x);
                displayRect.h = clamp(snap(displayRect.h + dy, 8), rotated ? minimum.w : minimum.h, output.height - displayRect.y);
            } else {
                displayRect.x = clamp(snap(displayRect.x + dx, 8), 0, output.width - displayRect.w);
                displayRect.y = clamp(snap(displayRect.y + dy, 8), 0, output.height - displayRect.h);
            }
            var logicalRect = unrotateRect(displayRect, size, page.rotation);
            card.rect = {
                x: Math.round(logicalRect.x), y: Math.round(logicalRect.y),
                w: Math.round(logicalRect.w), h: Math.round(logicalRect.h)
            };
            if (pointer.mode === 'resize') { card.size = 'custom'; this._spRenderCardSize(card); }
            var aligning = this._spNearAlignment(page, card);
            element.classList.toggle('is-aligning', aligning);
            element.classList.toggle('is-invalid', this._spRectCollision(page, card));
            this._spApplyCardRect(element, card, page, undefined, 1 - Math.min(0.3, 72 / output.height));
            ['x', 'y', 'w', 'h'].forEach(function(key) { var input = screenElement('sp-card-' + key); if (input) input.value = card.rect[key]; });
        },

        _spPointerUp(event) {
            var model = this._spEnsureModel();
            var pointer = model.pointer;
            if (!pointer || (event.pointerId !== undefined && event.pointerId !== pointer.pointerId)) return;
            model.pointer = null;
            screenElement('sp-editor-canvas')?.classList.remove('is-dragging');
            var page = this._spCurrentPage();
            var card = page.cards.find(function(item) { return item.id === pointer.cardId; });
            if (!pointer.moved || !card) { this._spRenderAll(); return; }
            model.suppressClickUntil = Date.now() + 250;
            if (event.type === 'pointercancel') {
                model.config = normalizeConfig(pointer.snapshot);
            } else if (this._spRectCollision(page, card)) {
                model.config = normalizeConfig(pointer.snapshot);
                this._spSetLayoutError('组件重叠，已恢复原位置');
            } else {
                model.undo.push(pointer.snapshot);
                if (model.undo.length > HISTORY_LIMIT) model.undo.shift();
                model.redo = [];
                this._spSetLayoutError('');
            }
            this._spMarkDirty();
        },

        _spRectCollision(page, card) {
            return page.cards.some(function(other) { return other.id !== card.id && rectsOverlap(card.rect, other.rect); });
        },

        _spNearAlignment(page, card) {
            return page.cards.some(function(other) {
                if (other.id === card.id || !other.rect) return false;
                var edges = [
                    Math.abs(card.rect.x - other.rect.x),
                    Math.abs(card.rect.y - other.rect.y),
                    Math.abs(card.rect.x + card.rect.w - other.rect.x - other.rect.w),
                    Math.abs(card.rect.y + card.rect.h - other.rect.y - other.rect.h)
                ];
                return edges.some(function(distance) { return distance <= 5; });
            });
        },

        _spLoadVisibleHistories(page) {
            var model = this._spEnsureModel();
            page.cards.filter(function(card) { return card.kind === 'line' && card.sourceRef; }).forEach((card) => {
                var ref = card.sourceRef, previous = model.histories[ref];
                if (previous && (previous.loading || Date.now() - previous.loadedAt < 8000)) return;
                var request = Object.assign({}, previous || { points: [] }, { loading: true });
                model.histories[ref] = request;
                var getter = typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
                getter('/api/periph-exec/history', { sourceRef: ref }).then((response) => {
                    if (!response || !response.success || !response.data) throw new Error('history unavailable');
                    if (model.histories[ref] === request) model.histories[ref] = Object.assign({}, response.data, { loadedAt: Date.now() });
                }).catch(() => {
                    if (model.histories[ref] === request) model.histories[ref] = Object.assign({}, previous || { points: [] }, { loading: false, loadedAt: Date.now(), stale: true });
                }).finally(() => { if (this.currentPage === 'screen-panel' && !model.pointer) this._spRenderSurface(screenElement('sp-editor-canvas'), this._spCurrentPage(), true); });
            });
        },

        _spReadApplication(state) {
            var model = this._spEnsureModel();
            model.application = Object.assign({}, state.application || state.panel || {});
            if (state.appliedRevision !== undefined) model.application.appliedRevision = state.appliedRevision;
            if (state.capabilities) model.capabilities = state.capabilities;
        },

        _spRefreshState(notify) {
            var model = this._spEnsureModel();
            if (model.weatherStatus) this._spWeatherStatus(model.weatherStatus);
            if (notify) Object.keys(model.histories).forEach(function(ref) { model.histories[ref].loadedAt = 0; });
            var getter = typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
            return getter('/api/screen-panel/state').then((response) => {
                if (response && response.success && response.data) {
                    model.state = response.data;
                    this._spReadApplication(response.data);
                    if (this.currentPage === 'screen-panel') {
                        this._spRenderPreview();
                        this._spApplyStatus();
                        this._spRenderSystemCapabilities();
                    }
                    if (notify) this._spSetMessage('状态已刷新', 'success');
                } else throw new Error('状态不可用');
                return response;
            }).catch((error) => {
                if (model.state.clock) model.state.clock.quality = 'offline';
                if (this.currentPage === 'screen-panel' && !(error && error._pageAborted)) {
                    this._spRenderPreview();
                }
                if (notify && !(error && error._pageAborted)) this._spSetMessage('刷新失败，保留上次数据', 'error');
                return null;
            });
        },

        _spExportConfig() {
            var model = this._spEnsureModel();
            var envelope = {
                storageVersion: 1,
                touchUiMigration: 2,
                revision: model.revision,
                config: sanitizeConfig(model.config, model.choices)
            };
            var blob = new Blob([JSON.stringify(envelope, null, 2) + '\n'], { type: 'application/json' });
            var url = URL.createObjectURL(blob);
            var link = document.createElement('a');
            link.href = url;
            link.download = 'screen-panel-layout.json';
            document.body.appendChild(link);
            link.click();
            link.remove();
            setTimeout(function() { URL.revokeObjectURL(url); }, 0);
            this._spSetMessage('布局已导出', 'success');
        },

        _spImportConfig(file) {
            var input = screenElement('sp-import-file');
            if (!file) return Promise.resolve(false);
            if (file.size > 64 * 1024) {
                this._spSetMessage('文件超过 64 KiB', 'error');
                if (input) input.value = '';
                return Promise.resolve(false);
            }
            return file.text().then((text) => {
                var parsed = JSON.parse(text);
                if (!parsed || typeof parsed !== 'object') throw new Error('文件不是 JSON 对象');
                if (parsed.storageVersion !== undefined && Number(parsed.storageVersion) !== 1) throw new Error('不支持的存储版本');
                var rawConfig = parsed.config && typeof parsed.config === 'object' ? parsed.config : parsed;
                if (Number(rawConfig.schemaVersion || 1) !== 1) throw new Error('不支持的布局 schemaVersion');
                if (!Array.isArray(rawConfig.pages) || rawConfig.pages.length === 0) throw new Error('布局必须至少包含一个页面');
                rawConfig.pages.forEach(function(page) {
                    if (!page || !Array.isArray(page.cards)) throw new Error('页面 cards 格式无效');
                    page.cards.forEach(function(card) {
                        if (!isSupportedCardInput(card)) throw new Error('包含不支持的组件类型');
                    });
                });
                var clean = normalizeConfig(rawConfig);
                var wireConfig = sanitizeConfig(clean, this._spEnsureModel().choices);
                var serialized = JSON.stringify(wireConfig);
                var bytes = typeof TextEncoder === 'function' ? new TextEncoder().encode(serialized).length : serialized.length;
                if (bytes > 20 * 1024) throw new Error('布局超过 20 KiB');
                var model = this._spEnsureModel();
                this._spPushUndo();
                model.config = clean;
                model.selectedPageId = clean.settings.homePageId || clean.pages[0].id;
                model.selectedCardId = '';
                this._spMarkDirty();
                var validation = this._spValidateConfig(clean);
                this._spSetMessage(validation.errors.length
                    ? ('草稿有 ' + validation.errors.length + ' 项错误：' + validation.errors[0])
                    : '布局已导入为草稿', validation.errors.length ? 'error' : 'success');
                return true;
            }).catch((error) => {
                this._spSetMessage('导入失败：' + (error && error.message || '文件格式无效'), 'error');
                return false;
            }).finally(function() {
                if (input) input.value = '';
            });
        },

        _spStartStatePolling() {
            var model = this._spEnsureModel();
            if (model.stateTimer) clearInterval(model.stateTimer);
            model.stateTimer = setInterval(() => {
                if (this.currentPage === 'screen-panel' && document.visibilityState !== 'hidden') this._spRefreshState(false);
            }, 8000);
        },

        _spStopStatePolling() {
            this._spOrderPointerUp({ type: 'pointercancel' });
            var model = this._spEnsureModel();
            if (model.stateTimer) clearInterval(model.stateTimer);
            model.stateTimer = null;
        },

        _spLoadAllChoices(getter) {
            var all = [];
            var seen = {};
            var capabilities = {};
            var loadPage = function(page) {
                return getter('/api/screen-panel/choices', { kind: 'all', page: page, pageSize: 50, q: '' }).then(function(response) {
                    var data = response && response.data || {};
                    if (page === 1 && data.capabilities && typeof data.capabilities === 'object') capabilities = data.capabilities;
                    unwrapList(response).forEach(function(item) {
                        var ref = choiceRef(item);
                        if (!ref || seen[ref]) return;
                        seen[ref] = true;
                        all.push(item);
                    });
                    if (data.hasMore === true && page < 8) return loadPage(page + 1);
                    return { items: all, capabilities: capabilities };
                });
            };
            return loadPage(1);
        },

        async _spSave() {
            var model = this._spEnsureModel();
            if (model.layoutSaving) return false;
            var validation = this._spValidateConfig(model.config);
            if (validation.errors.length) {
                this._spSetMessage(validation.errors[0] + (validation.errors.length > 1 ? ('；另有 ' + (validation.errors.length - 1) + ' 项') : ''), 'error');
                this._spSetLayoutError(validation.errors.join('；'));
                return Promise.resolve(false);
            }
            var cleanConfig = sanitizeConfig(model.config, model.choices);
            var serialized = JSON.stringify(cleanConfig);
            var payloadBytes = typeof TextEncoder === 'function' ? new TextEncoder().encode(serialized).length : serialized.length;
            if (payloadBytes > 20 * 1024) {
                this._spSetMessage('布局超过 20 KiB，请减少组件', 'error');
                return Promise.resolve(false);
            }
            var button = screenElement('sp-save-config');
            if (button) { button.disabled = true; button.textContent = '正在保存...'; }
            model.layoutSaving = true;
            var submittedConfig = clone(cleanConfig), baseConfig = sanitizeConfig(model.savedConfig, model.choices);
            var expectedRevision = model.revision, deviceMac = model.deviceMac;
            var response;
            function checked(result) {
                if (!result || result.success === false) {
                    var error = new Error(result && (result.error || result.message) || '保存失败');
                    error.status = result && result.code; error.data = result; throw error;
                }
                return result;
            }
            try {
                try {
                    response = checked(await apiPostJson('/api/screen-panel/config', { expectedRevision: expectedRevision, config: cleanConfig }));
                } catch (error) {
                    if (!error || error.status !== 409) throw error;
                    var getter = typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
                    var latest = checked(await getter('/api/screen-panel/config', { _t: Date.now() }));
                    var current = latest.data;
                    if (!current || !current.config || !Number.isInteger(current.revision) || current.revision < 1 || model.deviceMac !== deviceMac) throw error;
                    var remoteConfig = sanitizeConfig(current.config, model.choices);
                    var merged = helpers.mergeConfig(baseConfig, cleanConfig, remoteConfig);
                    if (merged.conflicts.length) {
                        model.configConflict = { revision: current.revision, config: clone(current.config), paths: merged.conflicts };
                        error.data = { currentRevision: current.revision };
                        throw error;
                    }
                    cleanConfig = merged.config;
                    var mergedValidation = this._spValidateConfig(normalizeConfig(cleanConfig));
                    if (mergedValidation.errors.length) throw new Error(mergedValidation.errors.join('；'));
                    serialized = JSON.stringify(cleanConfig);
                    if ((typeof TextEncoder === 'function' ? new TextEncoder().encode(serialized).length : serialized.length) > 20 * 1024) throw new Error('合并后的布局超过 20 KiB，请减少组件');
                    expectedRevision = current.revision;
                    // A lost response may be retried even though the first save succeeded.
                    response = helpers.configEqual(cleanConfig, remoteConfig) ? latest : checked(await apiPostJson('/api/screen-panel/config', {
                        expectedRevision: expectedRevision, config: cleanConfig
                    }));
                }
                if (model.deviceMac !== deviceMac) throw new Error('设备已切换，请重新读取配置');
                var data = response.data || {};
                var saved = sanitizeConfig(data.config || cleanConfig, model.choices);
                var pending = helpers.mergeConfig(submittedConfig, sanitizeConfig(model.config, model.choices), saved);
                model.revision = finiteNumber(data.revision, expectedRevision + 1);
                model.config = normalizeConfig(pending.config);
                model.savedConfig = normalizeConfig(saved);
                model.configConflict = null;
                model.application = data.application || model.application;
                model.dirty = !helpers.configEqual(sanitizeConfig(model.config, model.choices), saved); model.persisted = true;
                model.undo = [];
                model.redo = [];
                if (model.dirty) this._spPersistDraftSoon(); else this._spClearDraft();
                this._spSetMessage(model.dirty ? '配置已保存，保存期间的新修改仍保留在草稿' : data.application && data.application.pendingRestart ? '已保存，重启后应用到物理屏' : '配置已保存', 'success');
                this._spRenderAll();
                return true;
            } catch (error) {
                if (error && error.status === 409) {
                    var currentRevision = error.data && (error.data.currentRevision || error.data.data && error.data.data.currentRevision);
                    this._spSetMessage('设备配置已更新' + (currentRevision !== undefined ? ('（版本 ' + currentRevision + '）') : '') + '，存在冲突，草稿已保留。请先导出草稿，再刷新核对或撤销草稿读取设备配置。', 'error');
                } else if (!(error && error._pageAborted)) {
                    this._spSetMessage(error && error.data && (error.data.error || error.data.message) || error.message || '保存失败，请稍后重试', 'error');
                }
                return false;
            } finally {
                model.layoutSaving = false;
                if (button) { button.disabled = false; button.textContent = '保存并应用'; }
            }
        },

        loadScreenPanelPage(options) {
            options = options || {};
            var model = this._spEnsureModel();
            var loadId = model.loadId = (model.loadId || 0) + 1;
            this._spSetupEvents();
            this._spSetMessage('');
            var getter = options.noCache === true && typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
            var requests = [
                getter('/api/screen-panel/config'),
                this._spLoadAllChoices(getter),
                getter('/api/peripherals', { page: 1, pageSize: 50 }),
                getter('/api/screen-panel/state'),
                getter('/api/periph-exec/history/config'),
                getter('/api/screen-panel/weather'),
                (typeof apiGetFresh === 'function' ? apiGetFresh : apiGet)('/api/network/status', { _t: Date.now() })
            ];
            return Promise.allSettled(requests).then((results) => {
                if (loadId !== model.loadId) return null;
                var responses = results.map(function(result) { return result.status === 'fulfilled' ? result.value : null; });
                var configResponse = responses[0];
                if (!configResponse || configResponse.success === false || !configResponse.data) {
                    throw results[0].reason || new Error('屏幕配置不可用');
                }
                var payload = configResponse.data;
                var serverConfig = normalizeConfig(payload.config || null);
                var identity = responses[6];
                var mac = identity && identity.success !== false && identity.data ? String(identity.data.macAddress || '').toUpperCase() : '';
                if (!/^(?:[0-9A-F]{2}:){5}[0-9A-F]{2}$/.test(mac) || /^(00:){5}00$|^(FF:){5}FF$/.test(mac)) mac = '';
                var sameDevice = !!mac && model.deviceMac === mac;
                var keepSelection = sameDevice && options.preserveDraft !== false && model.loaded;
                if (!sameDevice) {
                    if (model.draftTimer) clearTimeout(model.draftTimer);
                    model.draftTimer = null;
                    model.flushDraft = null;
                    model.historyDirty = model.weatherDirty = false;
                    model.recoverableDraft = null;
                }
                model.deviceMac = mac;
                var keepDraft = sameDevice && options.preserveDraft !== false && model.loaded && model.dirty;
                if (!keepDraft) model.revision = finiteNumber(payload.revision, 0);
                model.persisted = !!payload.config && model.revision > 0;
                model.capabilities = payload.capabilities || {};
                model.application = payload.application || {};
                if (!keepDraft) {
                    model.savedConfig = clone(serverConfig);
                    model.configConflict = null;
                } else {
                    var merged = helpers.mergeConfig(sanitizeConfig(model.savedConfig, model.choices), sanitizeConfig(model.config, model.choices), sanitizeConfig(serverConfig, model.choices));
                    if (!merged.conflicts.length) {
                        model.config = normalizeConfig(merged.config);
                        model.savedConfig = clone(serverConfig);
                        model.revision = finiteNumber(payload.revision, model.revision);
                        model.configConflict = null;
                    } else model.configConflict = { revision: payload.revision, config: clone(serverConfig), paths: merged.conflicts };
                }
                var restored = options.preserveDraft !== false ? this._spRestoreDraft(serverConfig, model.revision) : null;
                if (!keepDraft) model.config = serverConfig;
                model.recoverableDraft = restored;
                model.dirty = !helpers.configEqual(sanitizeConfig(model.config, model.choices), sanitizeConfig(serverConfig, model.choices));
                if (!keepDraft && !keepSelection) model.selectedPageId = restored && model.config.pages.some(function(page) { return page.id === restored.selectedPageId; }) ? restored.selectedPageId :
                    (model.config.settings.homePageId || model.config.pages[0].id);
                if (!keepDraft && !keepSelection) model.selectedCardId = restored ? restored.selectedCardId || '' : '';
                var choicePayload = responses[1] || {};
                model.choices = Array.isArray(choicePayload.items) ? choicePayload.items : [];
                model.choiceCapabilities = choicePayload.capabilities || {};
                var peripherals = unwrapList(responses[2]);
                var capabilityDisplays = Array.isArray(model.capabilities.displays) ? model.capabilities.displays : [];
                model.displays = peripherals.filter(isDisplayPeripheral).concat(capabilityDisplays.filter(function(candidate) {
                    return !peripherals.some(function(item) { return String(item.id) === String(candidate.id || candidate.ref); });
                }));
                if (responses[3] && responses[3].success) {
                    model.state = responses[3].data || { items: {} };
                    this._spReadApplication(model.state);
                }
                if (responses[4] && responses[4].success && responses[4].data) {
                    if (!model.historyDirty || options.preserveDraft === false) { model.historyConfig = responses[4].data.config || null; model.historyDraft = clone(model.historyConfig); model.historyDirty = false; }
                    model.historyStatus = responses[4].data.status || null;
                } else {
                    model.historyConfig = null;
                    model.historyStatus = null;
                }
                if (responses[5] && responses[5].success) {
                    if (!model.weatherDirty || options.preserveDraft === false) { model.weather = responses[5].data; model.weatherDirty = false; this._spRenderWeather(); }
                } else { screenElement('sp-weather-status').textContent = '天气配置暂不可用'; }
                if (!model.config.displayRef && model.displays.length === 1) { model.config.displayRef = String(model.displays[0].id || model.displays[0].ref); model.dirty = true; }
                model.loaded = true;
                model.undo = [];
                model.redo = [];
                this._spRenderAll();
                if (typeof location !== 'undefined' && new URLSearchParams(location.search).get('tab') === 'history') {
                    this._spOpenProperties('history');
                }
                this._spStartStatePolling();
                if (keepDraft) {
                    this._spPersistDraftSoon();
                    if (model.configConflict) this._spSetMessage('设备配置已更新（版本 ' + payload.revision + '），与草稿有冲突。草稿已保留；可先导出草稿，再撤销草稿以读取设备配置。', 'error');
                }
                var recovery = screenElement('sp-draft-recovery'); if (recovery) recovery.classList.toggle('fb-hidden', !restored);
                return payload;
            }).catch((error) => {
                if (loadId !== model.loadId) return null;
                if (!(error && error._pageAborted)) {
                    this._spSetMessage('读取配置失败：' + (error && error.message || '设备暂不可用'), 'error');
                    model.config = model.config || defaultConfig();
                    this._spRenderAll();
                }
                return null;
            });
        }
    };

    Object.assign(methods, AppState._screenPanelSettingsMethods);
    if (typeof AppState.registerModule === 'function') AppState.registerModule('screen-panel', methods);
    else Object.assign(AppState, methods);

    if (typeof window !== 'undefined' && typeof window.addEventListener === 'function') {
        window.addEventListener('pagehide', function() { AppState._spFlushDraft(); });
    }
    if (typeof AppState.onPageHide === 'function') {
        AppState.onPageHide('screen-panel', function() {
            if (this.currentPage !== 'screen-panel') return;
            this._spStopStatePolling();
            this._spFlushDraft();
        });
    }
    if (typeof AppState.onPageShow === 'function') {
        AppState.onPageShow('screen-panel', function() {
            if (this.currentPage === 'screen-panel' && this._spModel && this._spModel.loaded) this._spStartStatePolling();
        });
    }
})();
