/**
 * Screen panel editor and read-only browser preview.
 * Physical controls are intentionally absent from this module.
 */
(function() {
    if (typeof AppState === 'undefined') return;

    var helpers = AppState._screenPanelModelHelpers;
    var STORAGE_KEY = 'fastbee.screenPanel.draft.v1';
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
                previewPageId: 'home',
                undo: [],
                redo: [],
                dirty: false,
                draftTimer: null,
                stateTimer: null,
                pointer: null,
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
            var element = document.getElementById('sp-page-message');
            if (!element) return;
            element.textContent = message || '';
            element.classList.toggle('fb-hidden', !message);
            element.classList.toggle('is-error', tone === 'error');
            element.classList.toggle('is-success', tone === 'success');
        },

        _spSetLayoutError(message) {
            var element = document.getElementById('sp-layout-error');
            if (!element) return;
            element.textContent = message || '';
            element.classList.toggle('fb-hidden', !message);
        },

        _spPersistDraftSoon() {
            var model = this._spEnsureModel();
            if (model.draftTimer) clearTimeout(model.draftTimer);
            model.draftTimer = setTimeout(function() {
                model.draftTimer = null;
                try {
                    if (!model.dirty) {
                        localStorage.removeItem(STORAGE_KEY);
                        return;
                    }
                    localStorage.setItem(STORAGE_KEY, JSON.stringify({
                        baseRevision: model.revision,
                        config: model.config,
                        selectedPageId: model.selectedPageId,
                        selectedCardId: model.selectedCardId,
                        updatedAt: Date.now()
                    }));
                } catch (_) { /* A full/disabled storage area must not block device saving. */ }
            }, 160);
        },

        _spClearDraft() {
            var model = this._spEnsureModel();
            if (model.draftTimer) clearTimeout(model.draftTimer);
            model.draftTimer = null;
            try { localStorage.removeItem(STORAGE_KEY); } catch (_) { /* ignore */ }
        },

        _spRestoreDraft(serverConfig, revision) {
            try {
                var raw = localStorage.getItem(STORAGE_KEY);
                if (!raw) return null;
                var draft = JSON.parse(raw);
                if (!draft || !draft.config || finiteNumber(draft.baseRevision, -1) !== finiteNumber(revision, 0)) return null;
                var normalized = normalizeConfig(draft.config);
                return {
                    config: normalized,
                    selectedPageId: draft.selectedPageId,
                    selectedCardId: draft.selectedCardId
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
            model.dirty = JSON.stringify(model.config) !== JSON.stringify(model.savedConfig);
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
            var dirty = document.getElementById('sp-dirty-state');
            if (dirty) {
                dirty.textContent = (model.dirty || model.weatherDirty) ? '有未保存修改' : '已保存';
                dirty.classList.toggle('is-dirty', model.dirty || model.weatherDirty);
            }
            var undo = document.getElementById('sp-undo');
            var redo = document.getElementById('sp-redo');
            if (undo) undo.disabled = model.undo.length === 0;
            if (redo) redo.disabled = model.redo.length === 0;
        },

        _spApplyStatus() {
            var model = this._spEnsureModel();
            var meta = displayStateMeta(model.application);
            var badge = document.getElementById('sp-runtime-badge');
            var summary = document.getElementById('sp-apply-summary');
            if (badge) {
                badge.className = 'status-badge ' + meta.className;
                badge.textContent = meta.pendingRestart && meta.text !== '待重启' ? meta.text + ' · 待重启' : meta.text;
            }
            if (summary) {
                var applied = model.application && (model.application.appliedRevision || model.application.revision);
                summary.textContent = meta.reason || (applied !== undefined ? ('设备已应用版本 ' + applied) : '可离线编辑');
            }
            var stateSummary = document.getElementById('sp-preview-state-summary');
            if (stateSummary) stateSummary.textContent = meta.reason || ('面板状态：' + meta.text);
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
            var canvasSize = document.getElementById('sp-canvas-size');
            if (canvasSize && surface.id === 'sp-editor-canvas') {
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

        _spLineSvg(card) {
            var history = this._spEnsureModel().histories[card.sourceRef] || {};
            var points = Array.isArray(history.points) ? history.points.slice(-10) : [];
            var values = points.map(function(point) { return Number(point.value); }).filter(Number.isFinite);
            if (values.length < 2) return '<div class="sp-card-meta">暂无历史记录</div>';
            var min = Math.min.apply(Math, values);
            var max = Math.max.apply(Math, values);
            if (min === max) { min -= 1; max += 1; }
            var coords = values.map(function(value, index) {
                var x = values.length === 1 ? 50 : index * 100 / (values.length - 1);
                var y = 92 - (value - min) * 78 / (max - min);
                return x.toFixed(1) + ',' + y.toFixed(1);
            }).join(' ');
            return '<svg class="sp-line-chart" viewBox="0 0 100 100" preserveAspectRatio="none" aria-hidden="true">' +
                '<line x1="0" y1="92" x2="100" y2="92"></line><line x1="0" y1="53" x2="100" y2="53"></line>' +
                '<polyline points="' + coords + '"></polyline></svg>';
        },

        _spBarSeriesContent(card, state) {
            var choices = this._spEnsureModel().choices;
            var sources = (card.sourceRefs || []).slice(0, 4);
            var units = sources.map(function(ref) {
                var series = (state.series || []).find(function(item) { return item.sourceRef === ref; }) || {};
                var choice = choices.find(function(item) { return choiceRef(item) === ref; });
                return String(series.unit !== undefined ? series.unit : choice && choice.unit || '');
            });
            return '<div class="sp-bar-series">' + sources.map((ref, index) => {
                var series = (state.series || []).find(function(item) { return item.sourceRef === ref; }) || {};
                var choice = choices.find(function(item) { return choiceRef(item) === ref; });
                var quality = String(series.quality || 'unknown').toLowerCase();
                var labels = { good: '正常', stale: '已过期', offline: '离线', error: '错误', unknown: '未知' };
                if (!labels[quality]) quality = 'unknown';
                var matchingUnit = units[index] === units[0];
                var value = series.value;
                var valid = matchingUnit && quality !== 'unknown' && value !== undefined && value !== null &&
                    typeof value !== 'boolean' && String(value).trim() !== '' && Number.isFinite(Number(value));
                var min = finiteNumber(card.min, 0), max = finiteNumber(card.max, 100);
                var percent = valid && max > min ? clamp((Number(value) - min) * 100 / (max - min), 0, 100) : 0;
                var status = matchingUnit ? labels[quality] : '单位不一致';
                if (matchingUnit && Number(series.updatedMs) > 0) status += ' · T+' + Math.floor((Number(series.updatedMs) >>> 0) / 1000) + 's';
                return '<div class="sp-bar-series-row" data-quality="' + (matchingUnit ? quality : 'error') + '">' +
                    '<div class="sp-bar-series-label"><span>' + escapeHtml(series.label || choice && choiceLabel(choice) || ref) + '</span><strong>' +
                    escapeHtml(valid ? this._spFormatValue(value, card.precision) + (units[index] ? ' ' + units[index] : '') : '--') + '</strong></div>' +
                    '<div class="sp-bar-track"><div class="sp-bar-fill" style="--sp-progress:' + percent.toFixed(1) + '%"></div></div>' +
                    '<div class="sp-card-meta">' + escapeHtml(status) + '</div></div>';
            }).join('') + '</div>';
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
            var footerRatio = editing ? 0 : 72 / rotatedSize(this._spReferenceSize(page, config), page.rotation).height;
            if (!editing) {
                var footer = document.createElement('nav'); footer.className = 'sp-device-footer';
                footer.style.height = (footerRatio * 100) + '%';
                var index = config.pages.indexOf(page);
                var navPages = config.pages.slice(0, 5);
                footer.style.gridTemplateColumns = 'repeat(' + Math.max(1, navPages.length) + ', minmax(0, 1fr))';
                footer.innerHTML = navPages.map(function(item) {
                    var active = item.id === page.id;
                    return '<button type="button"' + (active ? ' class="is-active" aria-current="page"' : '') +
                        ' data-sp-preview-page="' + escapeHtml(item.id) + '">' + escapeHtml(item.title) + '</button>';
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
            var container = document.getElementById('sp-page-tabs');
            if (!container) return;
            container.innerHTML = '';
            model.config.pages.forEach(function(page) {
                var button = document.createElement('button');
                button.type = 'button';
                button.className = 'sp-page-tab' + (page.id === model.selectedPageId ? ' is-active' : '');
                button.dataset.pageId = page.id;
                button.setAttribute('role', 'tab');
                button.setAttribute('aria-selected', page.id === model.selectedPageId ? 'true' : 'false');
                button.textContent = page.title;
                if (page.id === model.config.settings.homePageId) {
                    var home = document.createElement('span');
                    home.className = 'sp-home-mark';
                    home.textContent = '首页';
                    button.appendChild(home);
                }
                container.appendChild(button);
            });
            var currentIndex = model.config.pages.findIndex(function(page) { return page.id === model.selectedPageId; });
            var moveUp = document.getElementById('sp-page-up');
            var moveDown = document.getElementById('sp-page-down');
            if (moveUp) moveUp.disabled = currentIndex <= 0;
            if (moveDown) moveDown.disabled = currentIndex < 0 || currentIndex >= model.config.pages.length - 1;
        },

        _spRenderPreviewPagination(pageId, config) {
            var model = this._spEnsureModel();
            config = config || model.config;
            var index = Math.max(0, config.pages.findIndex(function(page) { return page.id === pageId; }));
            var pagination = document.getElementById('sp-preview-pagination');
            var previous = document.getElementById('sp-preview-prev');
            var next = document.getElementById('sp-preview-next');
            if (previous) previous.disabled = index <= 0;
            if (next) next.disabled = index >= config.pages.length - 1;
            if (!pagination) return;
            pagination.innerHTML = '';
            if (config.pages.length <= 1) return;
            config.pages.forEach(function(page, pageIndex) {
                var dot = document.createElement('button');
                dot.type = 'button';
                dot.className = 'sp-page-dot' + (pageIndex === index ? ' is-active' : '');
                dot.dataset.pageId = page.id;
                dot.setAttribute('aria-label', '预览 ' + page.title);
                pagination.appendChild(dot);
            });
        },

        _spPreviewConfig() {
            var source = document.getElementById('sp-preview-source');
            var model = this._spEnsureModel();
            return source && source.value === 'saved' ? model.savedConfig : model.config;
        },

        _spRenderPreview() {
            var model = this._spEnsureModel();
            var config = this._spPreviewConfig();
            var page = config.pages.find(function(item) { return item.id === model.previewPageId; }) ||
                config.pages.find(function(item) { return item.id === config.settings.homePageId; }) || config.pages[0];
            if (!page) return;
            model.previewPageId = page.id;
            var frame = document.getElementById('sp-preview-canvas');
            this._spRenderSurface(frame, page, false, config);
            this._spRenderPreviewPagination(page.id, config);
            var mode = document.getElementById('sp-preview-mode');
            var source = document.getElementById('sp-preview-source');
            if (mode) mode.textContent = source && source.value === 'saved' ? '显示设备已保存配置' : (model.dirty ? '显示当前未保存草稿' : '显示当前配置');
            this._spLoadVisibleHistories(page);
        },

        _spRenderSettings() {
            var model = this._spEnsureModel();
            var settings = model.config.settings;
            var setValue = function(id, value) { var el = document.getElementById(id); if (el) el.value = value; };
            setValue('sp-theme-id', settings.themeId || 'dark');
            setValue('sp-background-id', settings.background && settings.background.presetId || 'solid');
            setValue('sp-brightness', finiteNumber(settings.brightnessPercent, 70));
            setValue('sp-idle-sleep', finiteNumber(settings.idleSleepSeconds, 120));
            var output = document.getElementById('sp-brightness-output');
            var continuous = model.capabilities.brightnessContinuous !== false;
            var brightness = document.getElementById('sp-brightness'); if (brightness) brightness.disabled = !continuous;
            if (output) output.textContent = continuous ? (Number(settings.brightnessPercent) === 0 ? '最低亮度' : finiteNumber(settings.brightnessPercent, 70) + '%') : '仅支持背光开关';
            setValue('sp-confirm-idle', finiteNumber(settings.controlConfirmIdleSeconds, 60));
            var beep = document.getElementById('sp-touch-beep'); if (beep) beep.checked = settings.touchBeepEnabled !== false;
            var allowRestart = document.getElementById('sp-allow-restart');
            if (allowRestart) allowRestart.checked = settings.allowSystemRestart === true;
            this._spRenderSystemCapabilities();
            var displaySelect = document.getElementById('sp-display-ref');
            if (displaySelect) {
                var old = model.config.displayRef || '';
                displaySelect.innerHTML = '<option value="">未绑定屏幕</option>';
                model.displays.forEach(function(display) {
                    var option = document.createElement('option');
                    option.value = String(display.id || display.ref || '');
                    var params = display.params || display.config || display;
                    option.textContent = choiceLabel(display) + (params.width && params.height ? (' · ' + params.width + '×' + params.height) : '');
                    displaySelect.appendChild(option);
                });
                displaySelect.value = old;
            }
            var page = this._spCurrentPage();
            var pageLimit = document.getElementById('sp-page-limit');
            if (pageLimit) pageLimit.textContent = model.config.pages.length + '/' + this._spLimits().maxPages + ' 页 · 可增删排序，不固定三屏';
            setValue('sp-layout-template', page.layoutId || 'mixed-grid');
            document.querySelectorAll('[data-sp-layout-mode]').forEach(function(button) {
                button.classList.toggle('is-active', button.dataset.spLayoutMode === page.layoutMode);
            });
            this._spRenderHistorySettings();
        },

        _spSystemActionSupport(kind) {
            var capabilities = this._spEnsureModel().capabilities || {};
            var direct = { 'sleep-time': 'sleep', 'touch-beep': 'buzzer', 'confirm-idle': 'touch', brightness: 'brightnessContinuous', sleep: 'sleep', buzzer: 'buzzer', restart: 'systemRestart' };
            if (Object.prototype.hasOwnProperty.call(capabilities, direct[kind])) return capabilities[direct[kind]] !== false;
            var actions = capabilities.systemActions || capabilities.displayActions || {};
            if (kind === 'sleep-time') kind = 'sleep';
            if (Array.isArray(actions)) return actions.indexOf(kind) >= 0;
            return Object.prototype.hasOwnProperty.call(actions, kind) ? actions[kind] !== false : null;
        },

        _spRenderSystemCapabilities() {
            var container = document.getElementById('sp-system-capabilities');
            if (!container) return;
            container.innerHTML = SYSTEM_ACTION_KINDS.map((kind) => {
                var support = this._spSystemActionSupport(kind);
                var state = support === true ? '支持' : support === false ? '不支持' : '由显示设备决定';
                var stateClass = support === true ? 'is-supported' : support === false ? 'is-unsupported' : 'is-unknown';
                return '<span class="sp-capability-chip ' + stateClass + '"><strong>' + CARD_LABELS[kind] + '</strong>' + state + '</span>';
            }).join('');
        },

        _spRenderHistorySettings() {
            var model = this._spEnsureModel();
            var enabledInput = document.getElementById('sp-history-enabled');
            var sourceSelect = document.getElementById('sp-history-sources');
            var saveButton = document.getElementById('sp-history-save');
            var status = document.getElementById('sp-history-status');
            if (!enabledInput || !sourceSelect || !saveButton || !status) return;

            var capability = model.choiceCapabilities && model.choiceCapabilities.history || {};
            var supported = capability.supported !== false && !!model.historyConfig;
            var config = model.historyConfig || { enabled: false, sourceAllowlist: [] };
            var allowlist = Array.isArray(config.sourceAllowlist) ? config.sourceAllowlist.map(String) : [];
            var numericChoices = model.choices.filter(function(choice) {
                return !isControlChoice(choice) && String(choice && choice.type || '').toLowerCase() === 'number';
            });
            var known = {};
            numericChoices.forEach(function(choice) { known[choiceRef(choice)] = true; });

            sourceSelect.innerHTML = '';
            var allOption = document.createElement('option');
            allOption.value = '__all__';
            allOption.textContent = '全部显式开启记录的数值来源';
            allOption.selected = allowlist.length === 0;
            sourceSelect.appendChild(allOption);
            numericChoices.forEach(function(choice) {
                var option = document.createElement('option');
                option.value = choiceRef(choice);
                option.textContent = choiceLabel(choice);
                option.selected = allowlist.indexOf(option.value) >= 0;
                sourceSelect.appendChild(option);
            });
            allowlist.filter(function(ref) { return !known[ref]; }).forEach(function(ref) {
                var option = document.createElement('option');
                option.value = ref;
                option.textContent = ref + ' (当前不可用，保留)';
                option.selected = true;
                option.disabled = true;
                option.dataset.preserve = 'true';
                sourceSelect.appendChild(option);
            });

            enabledInput.checked = config.enabled === true;
            enabledInput.disabled = !supported || config.writesPaused === true;
            sourceSelect.disabled = !supported || config.writesPaused === true;
            saveButton.disabled = !supported || config.writesPaused === true;
            var persistence = model.historyStatus || {};
            status.textContent = !supported ? '当前固件未提供最近记录' : config.writesPaused === true ? '历史写入已暂停' :
                (config.enabled ? (persistence.persisted === false ? '已启用，配置待持久化' : '已启用，每个来源最多 10 条') : '已停用');
        },

        _spSaveHistorySettings() {
            if (!this.guardDeveloperModeAction()) return Promise.resolve(false);
            var model = this._spEnsureModel();
            var enabledInput = document.getElementById('sp-history-enabled');
            var sourceSelect = document.getElementById('sp-history-sources');
            var button = document.getElementById('sp-history-save');
            if (!enabledInput || !sourceSelect || !model.historyConfig) return Promise.resolve(false);
            var selected = Array.from(sourceSelect.options).filter(function(option) {
                return option.selected;
            }).map(function(option) { return option.value; });
            var allSources = selected.indexOf('__all__') >= 0;
            var sourceAllowlist = allSources ? [] : selected.filter(function(ref) { return ref !== '__all__'; });
            Array.from(sourceSelect.options).filter(function(option) {
                return option.dataset.preserve === 'true';
            }).forEach(function(option) {
                if (!allSources && sourceAllowlist.indexOf(option.value) < 0) sourceAllowlist.push(option.value);
            });
            var maxSources = finiteNumber(model.historyConfig.maxSources,
                finiteNumber(model.choiceCapabilities && model.choiceCapabilities.history && model.choiceCapabilities.history.maxSources, 16));
            if (sourceAllowlist.length > maxSources) {
                this._spSetMessage('最近记录来源超过上限 ' + maxSources, 'error');
                return Promise.resolve(false);
            }
            if (button) { button.disabled = true; button.textContent = '正在保存...'; }
            return apiPostJson('/api/periph-exec/history/config', {
                enabled: enabledInput.checked === true,
                sourceAllowlist: sourceAllowlist
            }).then((response) => {
                if (!response || response.success === false || !response.data) throw new Error(response && response.message || '保存失败');
                model.historyConfig = response.data.config || model.historyConfig;
                model.historyStatus = response.data.status || model.historyStatus;
                model.histories = {};
                this._spRenderHistorySettings();
                this._spRenderAll();
                this._spSetMessage('最近记录设置已保存', 'success');
                if (typeof window.apiInvalidateCache === 'function') window.apiInvalidateCache('/api/periph-exec/history/config');
                return true;
            }).catch((error) => {
                if (!(error && error._pageAborted)) this._spSetMessage(error && error.message || '最近记录设置保存失败', 'error');
                return false;
            }).finally(() => {
                if (button) button.textContent = '保存记录设置';
                this._spRenderHistorySettings();
            });
        },

        _spRenderInspector() {
            var model = this._spEnsureModel();
            var card = this._spCurrentCard();
            var empty = document.getElementById('sp-inspector-empty');
            var form = document.getElementById('sp-inspector-form');
            if (!empty || !form) return;
            empty.classList.toggle('fb-hidden', !!card);
            form.classList.toggle('fb-hidden', !card);
            if (!card) return;
            var kind = document.getElementById('sp-inspector-kind');
            var id = document.getElementById('sp-inspector-id');
            var title = document.getElementById('sp-card-title');
            if (kind) kind.textContent = CARD_LABELS[card.kind] || card.kind;
            if (id) id.textContent = card.id.slice(-6);
            if (title) title.value = card.title || '';

            var sourceField = document.getElementById('sp-source-field');
            var seriesField = document.getElementById('sp-bar-sources-field');
            var controlField = document.getElementById('sp-control-field');
            var pageField = document.getElementById('sp-page-link-field');
            var displayField = document.getElementById('sp-display-field');
            var numeric = document.getElementById('sp-numeric-fields');
            var stepField = document.getElementById('sp-card-step-field');
            var initialField = document.getElementById('sp-card-initial-field');
            var confirmField = document.getElementById('sp-confirm-field');
            var usesSource = SOURCE_KINDS.indexOf(card.kind) >= 0;
            if (sourceField) sourceField.classList.toggle('fb-hidden', !usesSource);
            if (seriesField) seriesField.classList.toggle('fb-hidden', card.kind !== 'bar');
            if (controlField) controlField.classList.toggle('fb-hidden', card.kind !== 'control' && card.kind !== 'motor');
            if (pageField) pageField.classList.toggle('fb-hidden', card.kind !== 'page-link');
            var motorFields = document.getElementById('sp-motor-fields');
            if (motorFields) motorFields.classList.toggle('fb-hidden', card.kind !== 'motor');
            [['speed','motorSpeed'], ['pulse','motorPulse'], ['speed-step','motorSpeedStep'], ['pulse-step','motorPulseStep']].forEach(function(pair) {
                var field = document.getElementById('sp-motor-' + pair[0]); if (field) field.value = card[pair[1]] === undefined ? CARD_DEFAULTS.motor[pair[1]] : card[pair[1]];
            });
            var numericControl = card.kind === 'control' && card.display === 'number';
            if (numeric) numeric.classList.toggle('fb-hidden', ['data', 'gauge', 'bar', 'line'].indexOf(card.kind) < 0 && !numericControl);
            if (stepField) stepField.classList.toggle('fb-hidden', !numericControl);
            if (initialField) initialField.classList.toggle('fb-hidden', !numericControl);
            if (confirmField) confirmField.classList.toggle('fb-hidden', ['control'].concat(SYSTEM_ACTION_KINDS).indexOf(card.kind) < 0);

            var dataChoices = model.choices.filter(function(item) {
                if (card.kind === 'weather') return isWeatherChoice(item);
                if (card.kind === 'bar') return helpers.isNumericChoice(item);
                return !isControlChoice(item) && !isWeatherChoice(item) && String(item.kind || '').toLowerCase() !== 'system';
            });
            this._spChoiceOptions(document.getElementById('sp-card-source'), dataChoices, card.sourceRef, card.kind === 'weather' ? '请选择天气来源' : '请选择数据来源');
            for (var seriesIndex = 1; seriesIndex < 4; seriesIndex++) {
                this._spChoiceOptions(document.getElementById('sp-card-source-' + seriesIndex), dataChoices,
                    card.sourceRefs && card.sourceRefs[seriesIndex] || '', '不添加对比来源');
            }
            this._spChoiceOptions(document.getElementById('sp-card-control'), model.choices.filter(function(choice) { return card.kind === 'motor' ? choice.type === 'motor' : isControlChoice(choice) && choice.type !== 'motor'; }), card.controlRef, '请选择控制项');

            var pageSelect = document.getElementById('sp-card-target-page');
            if (pageSelect) {
                pageSelect.innerHTML = '';
                model.config.pages.forEach(function(page) {
                    var option = document.createElement('option');
                    option.value = page.id;
                    option.textContent = page.title;
                    pageSelect.appendChild(option);
                });
                pageSelect.value = card.targetPageId || '';
            }

            var display = document.getElementById('sp-card-display');
            if (display) {
                var displayOptions = {
                    motor: [['motor', '电机控制']], 'touch-beep': [['touch-beep', '触摸音']], 'confirm-idle': [['confirm-idle', '操作确认']], 'sleep-time': [['sleep-time', '自动息屏']], data: [['value', '数值']], status: [['status', '状态']], gauge: [['gauge', '仪表/进度']],
                    bar: [['bar', '当前值柱状']], line: [['recent10', '最近10点折线']], control: [['switch', '开关'], ['action', '执行一次 / 场景'], ['number', '数值调节并执行']],
                    clock: [['clock', '时间与日期']], weather: [['weather', '天气概览']],
                    'page-link': [['page-link', '页面入口']], brightness: [['brightness', '设置亮度']],
                    sleep: [['sleep', '立即息屏']], buzzer: [['buzzer', '蜂鸣器短响']], restart: [['restart', '重启按钮']]
                };
                display.innerHTML = (displayOptions[card.kind] || []).map(function(option) {
                    return '<option value="' + option[0] + '">' + option[1] + '</option>';
                }).join('');
                display.value = card.display || '';
            }
            var values = {
                'sp-card-unit': card.unit || '', 'sp-card-precision': finiteNumber(card.precision, 1),
                'sp-card-min': finiteNumber(card.min, 0), 'sp-card-max': finiteNumber(card.max, 100),
                'sp-card-step': finiteNumber(card.step, 1), 'sp-card-initial': finiteNumber(card.initialValue, card.min || 0),
                'sp-card-size': card.size || 'small'
            };
            Object.keys(values).forEach(function(key) { var input = document.getElementById(key); if (input) input.value = values[key]; });
            var confirm = document.getElementById('sp-card-confirm');
            if (confirm) confirm.checked = card.confirm === true;
            if (displayField) displayField.classList.toggle('fb-hidden', false);
        },

        _spRenderMobileList() {
            var page = this._spCurrentPage();
            var container = document.getElementById('sp-mobile-card-list');
            if (!container || !page) return;
            container.innerHTML = '';
            page.cards.forEach(function(card, index) {
                var row = document.createElement('div');
                row.className = 'sp-mobile-card-row';
                row.dataset.cardId = card.id;
                row.innerHTML = '<div><strong>' + escapeHtml(card.title || CARD_LABELS[card.kind]) + '</strong><small>' +
                    escapeHtml(CARD_LABELS[card.kind] || card.kind) + ' · ' + escapeHtml(card.size || 'small') + '</small></div>' +
                    '<div class="sp-mobile-card-actions">' +
                    '<button type="button" class="fb-btn fb-btn-sm" data-sp-card-move="up" aria-label="上移"' + (index === 0 ? ' disabled' : '') + '>&uarr;</button>' +
                    '<button type="button" class="fb-btn fb-btn-sm" data-sp-card-move="down" aria-label="下移"' + (index === page.cards.length - 1 ? ' disabled' : '') + '>&darr;</button>' +
                    '<button type="button" class="fb-btn fb-btn-sm" data-sp-card-select="true">设置</button></div>';
                container.appendChild(row);
            });
        },

        _spRenderAll() {
            var model = this._spEnsureModel();
            if (!document.getElementById('screen-panel-page')) return;
            if (!model.config.pages.some(function(page) { return page.id === model.selectedPageId; })) model.selectedPageId = model.config.pages[0].id;
            var page = this._spCurrentPage();
            this._spEnsurePageRects(page);
            this._spRenderSettings();
            this._spRenderPageTabs();
            this._spRenderContentList();
            this._spRenderSurface(document.getElementById('sp-editor-canvas'), page, true);
            this._spRenderInspector();
            this._spRenderMobileList();
            this._spRenderPreview();
            this._spRenderDirtyState();
            this._spApplyStatus();
            var validation = this._spValidateConfig(model.config);
            var summary = document.getElementById('sp-validation-summary');
            if (summary) summary.textContent = validation.errors.length ? (validation.errors.length + ' 项需要处理') : (model.config.pages.length + ' 页 · ' + validation.cardCount + ' 个组件');
        },

        _spValidateConfig(config) {
            return helpers.validateConfig(config, this);
        },

        _spSetupEvents() {
            var root = document.getElementById('screen-panel-page');
            if (!root || this._screenPanelEventsRoot === root) return;
            this._screenPanelEventsRoot = root;

            root.addEventListener('click', (event) => {
                var adjust = event.target.closest('[data-sp-adjust-page]');
                if (adjust) { this._spEnsureModel().selectedPageId = adjust.dataset.spAdjustPage; document.getElementById('sp-layout-editor').open = true; this._spRenderAll(); document.getElementById('sp-layout-editor').scrollIntoView({ block: 'start' }); return; }
                var tab = event.target.closest('.sp-top-tabs [data-tab]');
                if (tab) { this._spSwitchTopTab(tab.dataset.tab); return; }
                var nav = event.target.closest('[data-sp-nav]');
                if (nav) { this._spNavigate(nav.dataset.spNav, nav.dataset.spTargetTab); return; }
                var pageTab = event.target.closest('.sp-page-tab[data-page-id]');
                if (pageTab) {
                    this._spEnsureModel().selectedPageId = pageTab.dataset.pageId;
                    this._spEnsureModel().selectedCardId = '';
                    this._spRenderAll();
                    return;
                }
                var dot = event.target.closest('.sp-page-dot[data-page-id]');
                if (dot) { this._spEnsureModel().previewPageId = dot.dataset.pageId; this._spRenderPreview(); return; }
                var previewPage = event.target.closest('[data-sp-preview-page]');
                if (previewPage) { this._spEnsureModel().previewPageId = previewPage.dataset.spPreviewPage; this._spRenderPreview(); return; }
                var add = event.target.closest('[data-sp-add]');
                if (add) { this._spAddCard(add.dataset.spAdd); return; }
                var mode = event.target.closest('[data-sp-layout-mode]');
                if (mode) { this._spChangeLayoutMode(mode.dataset.spLayoutMode); return; }
                var cardElement = event.target.closest('#sp-editor-canvas .sp-panel-card[data-card-id]');
                if (cardElement && !event.target.closest('.sp-resize-handle')) {
                    this._spEnsureModel().selectedCardId = cardElement.dataset.cardId;
                    this._spRenderAll();
                    return;
                }
                var mobileRow = event.target.closest('.sp-mobile-card-row[data-card-id]');
                if (mobileRow) {
                    if (event.target.closest('[data-sp-card-select]')) {
                        this._spEnsureModel().selectedCardId = mobileRow.dataset.cardId;
                        this._spRenderAll();
                    } else {
                        var move = event.target.closest('[data-sp-card-move]');
                        if (move) this._spMoveCard(mobileRow.dataset.cardId, move.dataset.spCardMove);
                    }
                    return;
                }
                switch (event.target.id) {
                    case 'sp-refresh-btn': this.loadScreenPanelPage({ noCache: true, preserveDraft: true }); break;
                    case 'sp-preview-refresh-btn': this._spRefreshState(true); break;
                    case 'sp-preview-home': this._spEnsureModel().previewPageId = this._spPreviewConfig().settings.homePageId; this._spRenderPreview(); break;
                    case 'sp-footer-prev': case 'sp-preview-prev': this._spMovePreview(-1); break;
                    case 'sp-footer-next': case 'sp-preview-next': this._spMovePreview(1); break;
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
                    case 'sp-import-config': document.getElementById('sp-import-file')?.click(); break;
                    case 'sp-export-config': this._spExportConfig(); break;
                    case 'sp-reset-draft': this._spResetDraft(); break;
                    case 'sp-history-save': this._spSaveHistorySettings(); break;
                    case 'sp-save-config': this._spSaveAll(); break;
                    case 'sp-weather-refresh': this._spRefreshWeather(); break;
                    case 'sp-edit-current': this._spEnsureModel().selectedPageId = this._spEnsureModel().previewPageId; this._spSwitchTopTab('sp-config'); this._spRenderAll(); break;
                }
            });

            root.addEventListener('change', (event) => {
                if (event.target.id.startsWith('sp-weather-')) {
                    this._spEnsureModel().weatherDirty = true;
                    this._spRenderDirtyState(); return;
                }
                if (event.target.matches('[data-sp-content-source]')) {
                    var id = event.target.dataset.spContentSource, ref = event.target.value;
                    this._spMutate((config) => config.pages.forEach((page) => {
                        page.cards.forEach((card) => {
                            if (card.id !== id) return;
                            var choice = this._spEnsureModel().choices.find(function(item) { return choiceRef(item) === ref; });
                            if (card.kind === 'motor' || card.kind === 'control') card.controlRef = ref;
                            else { card.sourceRef = ref; if (choice) { card.unit = choice.unit || ''; card.precision = finiteNumber(choice.precision, 1); card.title = choiceLabel(choice); } }
                        });
                    })); return;
                }
                if (event.target.id === 'sp-preview-source') { this._spRenderPreview(); return; }
                if (event.target.id === 'sp-card-control') {
                    var selected = this._spEnsureModel().choices.find(function(choice) {
                        return choiceRef(choice) === event.target.value;
                    });
                    var selectedType = String(selected && selected.type || '').toLowerCase();
                    var displaySelect = document.getElementById('sp-card-display');
                    if (displaySelect && ['switch', 'action', 'number'].indexOf(selectedType) >= 0) {
                        displaySelect.value = selectedType;
                    }
                    var numericFields = document.getElementById('sp-numeric-fields');
                    var isNumber = selectedType === 'number';
                    if (numericFields) numericFields.classList.toggle('fb-hidden', !isNumber);
                    ['sp-card-step-field', 'sp-card-initial-field'].forEach(function(id) {
                        document.getElementById(id)?.classList.toggle('fb-hidden', !isNumber);
                    });
                    if (isNumber && this._spCurrentCard()?.display !== 'number') {
                        var defaults = {
                            'sp-card-unit': selected.unit || '',
                            'sp-card-precision': finiteNumber(selected.precision, 0),
                            'sp-card-min': finiteNumber(selected.rangeMin, 0),
                            'sp-card-max': finiteNumber(selected.rangeMax, 100),
                            'sp-card-step': finiteNumber(selected.step, 1),
                            'sp-card-initial': finiteNumber(selected.initialValue, 0)
                        };
                        Object.keys(defaults).forEach(function(id) {
                            var field = document.getElementById(id);
                            if (field) field.value = defaults[id];
                        });
                    }
                    return;
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
                var outputMap = { 'sp-brightness': 'sp-brightness-output' };
                var output = document.getElementById(outputMap[event.target.id]);
                if (output) output.textContent = Number(event.target.value) === 0 ? '最低亮度' : event.target.value + '%';
            });
            var brightness = document.getElementById('sp-brightness');
            if (brightness) brightness.addEventListener('change', (event) => {
                this._spMutate(function(config) { config.settings.brightnessPercent = finiteNumber(event.target.value, 70); });
            });
            var editor = document.getElementById('sp-layout-editor');
            if (editor) editor.addEventListener('toggle', () => {
                if (editor.open) this._spMountInspector();
            });
            root.addEventListener('submit', (event) => {
                if (event.target.id === 'sp-inspector-form') { event.preventDefault(); this._spApplyInspector(); }
            });
            var importFile = document.getElementById('sp-import-file');
            if (importFile) importFile.addEventListener('change', (event) => this._spImportConfig(event.target.files && event.target.files[0]));
            var canvas = document.getElementById('sp-editor-canvas');
            if (canvas) {
                canvas.addEventListener('pointerdown', (event) => this._spPointerDown(event));
                canvas.addEventListener('keydown', (event) => this._spCanvasKeydown(event));
            }
            var previewCanvas = document.getElementById('sp-preview-canvas');
            if (previewCanvas) previewCanvas.addEventListener('keydown', (event) => {
                if (event.key === 'ArrowLeft' || event.key === 'ArrowRight') {
                    event.preventDefault();
                    this._spMovePreview(event.key === 'ArrowLeft' ? -1 : 1);
                }
            });
            root.addEventListener('keydown', (event) => {
                var topTab = event.target.closest('.sp-top-tabs [role="tab"]');
                if (topTab && (event.key === 'ArrowLeft' || event.key === 'ArrowRight')) {
                    var target = document.getElementById(topTab.id === 'sp-tab-preview' ? 'sp-tab-config' : 'sp-tab-preview');
                    event.preventDefault();
                    this._spSwitchTopTab(target.dataset.tab);
                    target.focus();
                    return;
                }
                if ((event.ctrlKey || event.metaKey) && event.key.toLowerCase() === 'z') {
                    event.preventDefault();
                    if (event.shiftKey) this._spRedo(); else this._spUndo();
                }
            });
        },

        _spSwitchTopTab(tabId) {
            var root = document.getElementById('screen-panel-page');
            if (!root) return;
            root.querySelectorAll('.sp-top-tabs [data-tab]').forEach(function(button) {
                var active = button.dataset.tab === tabId;
                button.classList.toggle('active', active);
                button.setAttribute('aria-selected', active ? 'true' : 'false');
                button.tabIndex = active ? 0 : -1;
            });
            ['sp-preview', 'sp-config'].forEach(function(id) {
                var section = document.getElementById(id);
                if (!section) return;
                var active = id === tabId;
                section.classList.toggle('active', active);
                section.hidden = !active;
            });
            if (tabId === 'sp-preview') this._spRenderPreview();
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

        _spGenerateThreeScreenTemplate() {
            var model = this._spEnsureModel();
            var limits = this._spLimits();
            if (limits.maxPages < 4 || limits.maxCards < 5) {
                this._spSetMessage('资源不足，请单独添加页面', 'error');
                return;
            }
            var hasContent = model.config.pages.length > 1 || model.config.pages.some(function(page) { return page.cards.length > 0; });
            if (hasContent && !window.confirm('替换当前草稿为起始界面？保存后生效。')) return;

            var current = this._spCurrentPage();
            var size = this._spReferenceSize(current);
            var rotation = current && current.rotation || 0;
            var config = helpers.buildThreeScreenTemplate(model.config, model.choices, size, rotation);
            config.pages.forEach((page) => this._spLayoutTemplatePage(page, config));
            this._spPushUndo();
            model.config = config;
            model.selectedPageId = 'home';
            model.previewPageId = 'home';
            model.selectedCardId = '';
            this._spMarkDirty();
            this._spSetMessage('起始界面已生成', 'success');
        },

        _spAddPage() {
            var model = this._spEnsureModel();
            var limits = this._spLimits();
            if (model.config.pages.length >= limits.maxPages) {
                this._spSetMessage('页面数量已达到设备上限 ' + limits.maxPages, 'error');
                return;
            }
            var preset = document.getElementById('sp-page-preset');
            var template = preset && preset.value || 'blank';
            var id = safeId('page');
            var currentPage = this._spCurrentPage();
            var newPage = helpers.buildPageTemplate(template, model.config, model.choices,
                this._spReferenceSize(currentPage), currentPage.rotation || 0, id);
            var existingCards = model.config.pages.reduce(function(total, page) { return total + page.cards.length; }, 0);
            if (newPage.cards.length > limits.maxCardsPerPage || existingCards + newPage.cards.length > limits.maxCards) {
                this._spSetMessage('组件超限，请删除组件或添加空白页', 'error');
                return;
            }
            var title = window.prompt('页面名称', newPage.title);
            if (title === null) return;
            newPage.title = title.trim().slice(0, 64) || newPage.title;
            this._spLayoutTemplatePage(newPage, model.config);
            this._spMutate(function(config) {
                config.pages.push(newPage);
                model.selectedPageId = id;
                model.previewPageId = id;
                model.selectedCardId = '';
            });
        },

        _spRenamePage() {
            var page = this._spCurrentPage();
            var title = window.prompt('页面名称', page.title);
            if (title === null) return;
            title = title.trim().slice(0, 64);
            if (!title || title === page.title) return;
            this._spMutate(function() { page.title = title; });
        },

        _spSetHomePage() {
            var page = this._spCurrentPage();
            this._spMutate(function(config) { config.settings.homePageId = page.id; });
        },

        _spMovePage(direction) {
            var model = this._spEnsureModel();
            var index = model.config.pages.findIndex(function(page) { return page.id === model.selectedPageId; });
            var target = index + direction;
            if (index < 0 || target < 0 || target >= model.config.pages.length) return;
            this._spMutate(function(config) {
                var page = config.pages.splice(index, 1)[0];
                config.pages.splice(target, 0, page);
            });
        },

        _spDeletePage() {
            var model = this._spEnsureModel();
            var page = this._spCurrentPage();
            if (model.config.pages.length <= 1) {
                this._spSetMessage('至少保留一个页面', 'error');
                return;
            }
            if (!window.confirm('删除“' + page.title + '”？其中组件会一并移除。')) return;
            this._spMutate(function(config) {
                config.pages = config.pages.filter(function(item) { return item.id !== page.id; });
                config.pages.forEach(function(item) {
                    item.cards.forEach(function(card) { if (card.targetPageId === page.id) card.targetPageId = ''; });
                });
                if (config.settings.homePageId === page.id) config.settings.homePageId = config.pages[0].id;
                model.selectedPageId = config.pages[0].id;
                model.previewPageId = config.pages[0].id;
                model.selectedCardId = '';
            });
        },

        _spAddCard(kind) {
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
                this._spSwitchTopTab('sp-config');
                this._spSetMessage('请先在配置中开启重启许可', 'error');
                return;
            }
            var systemSupport = SYSTEM_ACTION_KINDS.indexOf(kind) >= 0 ? this._spSystemActionSupport(kind) : null;
            if (systemSupport === false) {
                this._spSetMessage('当前显示设备不支持“' + CARD_LABELS[kind] + '”操作', 'error');
                return;
            }
            var historyRequired = kind === 'line' && (!model.historyConfig || model.historyConfig.enabled !== true);
            if (historyRequired) {
                this._spSwitchTopTab('sp-config');
            }
            var card = Object.assign({
                id: safeId('card'), kind: kind, sourceRef: '', controlRef: '', targetPageId: '',
                unit: '', precision: 1, min: 0, max: 100, step: 1, initialValue: 0,
                confirm: false
            }, clone(CARD_DEFAULTS[kind]));
            if (kind === 'motor') card.controlRef = choiceRef(model.choices.find(function(choice) { return choice.type === 'motor'; }));
            if (executeOnce) { card.title = '执行一次'; card.display = 'action'; }
            if (kind === 'brightness') card.actionValue = clamp(Math.round(finiteNumber(model.config.settings.brightnessPercent, 70)), 0, 100);
            if (kind === 'page-link') card.targetPageId = model.config.pages.find(function(item) { return item.id !== page.id; })?.id || page.id;
            this._spMutate(() => {
                page.cards.push(card);
                this._spLayoutTemplatePage(page);
                model.selectedCardId = card.id;
            });
            this._spSetMessage(historyRequired ?
                '折线已添加，保存时自动启用所选来源的最近记录。' :
                ('已添加“' + card.title + '”，请在右侧选择绑定内容。'), historyRequired ? 'error' : 'success');
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

        _spApplyInspector() {
            var card = this._spCurrentCard();
            var page = this._spCurrentPage();
            if (!card) return;
            var get = function(id) { return document.getElementById(id); };
            var barRefs = [];
            if (card.kind === 'bar') {
                barRefs = [get('sp-card-source')?.value || '', get('sp-card-source-1')?.value || '',
                    get('sp-card-source-2')?.value || '', get('sp-card-source-3')?.value || ''].filter(Boolean);
                var selectedChoices = barRefs.map((ref) => this._spEnsureModel().choices.find(function(choice) { return choiceRef(choice) === ref; }));
                if ((barRefs.length && !get('sp-card-source')?.value) || new Set(barRefs).size !== barRefs.length ||
                    selectedChoices.some(function(choice) { return !helpers.isNumericChoice(choice); }) ||
                    selectedChoices.some(function(choice) { return String(choice.unit || '') !== String(selectedChoices[0].unit || ''); })) {
                    this._spSetMessage('请选择不重复的同单位数值来源，并先设置数据来源', 'error');
                    return;
                }
            }
            this._spMutate(() => {
                card.title = String(get('sp-card-title')?.value || CARD_LABELS[card.kind]).trim().slice(0, 64);
                card.sourceRef = get('sp-card-source')?.value || '';
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
                card.size = get('sp-card-size')?.value || 'small';
                if (card.kind === 'control' && card.display === 'number' && card.size === 'small') card.size = 'large';
                card.confirm = card.kind === 'motor' || get('sp-card-confirm')?.checked === true;
                if (page.layoutMode === 'template') this._spLayoutTemplatePage(page);
            });
            this._spSetMessage('组件设置已应用到草稿', 'success');
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

        _spApplyTemplate() {
            var page = this._spCurrentPage();
            if (page.cards.length > 0 && !window.confirm('将按当前模板重新排列组件，绑定内容不会改变。继续吗？')) return;
            this._spMutate(() => { page.layoutMode = 'template'; this._spLayoutTemplatePage(page); });
        },

        _spMoveCard(cardId, direction) {
            var page = this._spCurrentPage();
            var index = page.cards.findIndex(function(card) { return card.id === cardId; });
            var target = direction === 'up' ? index - 1 : index + 1;
            if (index < 0 || target < 0 || target >= page.cards.length) return;
            this._spMutate(() => {
                var moved = page.cards.splice(index, 1)[0];
                page.cards.splice(target, 0, moved);
                this._spLayoutTemplatePage(page);
            });
        },

        _spUndo() {
            var model = this._spEnsureModel();
            if (!model.undo.length) return;
            model.redo.push(clone(model.config));
            model.config = normalizeConfig(model.undo.pop());
            if (!model.config.pages.some(function(page) { return page.id === model.selectedPageId; })) model.selectedPageId = model.config.pages[0].id;
            model.selectedCardId = '';
            this._spMarkDirty();
        },

        _spRedo() {
            var model = this._spEnsureModel();
            if (!model.redo.length) return;
            model.undo.push(clone(model.config));
            model.config = normalizeConfig(model.redo.pop());
            if (!model.config.pages.some(function(page) { return page.id === model.selectedPageId; })) model.selectedPageId = model.config.pages[0].id;
            model.selectedCardId = '';
            this._spMarkDirty();
        },

        _spResetDraft() {
            var model = this._spEnsureModel();
            if ((model.dirty || model.weatherDirty) && !window.confirm('撤销全部未保存修改？')) return;
            model.weatherDirty = false; this._spRenderWeather();
            model.config = normalizeConfig(model.savedConfig);
            model.selectedPageId = model.config.settings.homePageId || model.config.pages[0].id;
            model.previewPageId = model.selectedPageId;
            model.selectedCardId = '';
            model.undo = [];
            model.redo = [];
            model.dirty = false;
            this._spClearDraft();
            this._spSetMessage('已恢复设备保存的配置', 'success');
            this._spRenderAll();
        },

        _spMovePreview(direction) {
            var model = this._spEnsureModel();
            var config = this._spPreviewConfig();
            var index = config.pages.findIndex(function(page) { return page.id === model.previewPageId; });
            index = clamp(index + direction, 0, config.pages.length - 1);
            model.previewPageId = config.pages[index].id;
            this._spRenderPreview();
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
                this._spSetLayoutError('移动后会与其他组件重叠，已保留原位置');
            } else {
                this._spSetLayoutError('');
            }
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
            var logicalSize = this._spReferenceSize(page);
            model.selectedCardId = card.id;
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
            var canvas = document.getElementById('sp-editor-canvas');
            canvas?.classList.add('is-dragging');
            var move = (moveEvent) => this._spPointerMove(moveEvent);
            var up = (upEvent) => {
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
            var canvas = document.getElementById('sp-editor-canvas');
            var element = canvas && Array.prototype.find.call(canvas.querySelectorAll('.sp-panel-card[data-card-id]'), function(candidate) {
                return candidate.dataset.cardId === pointer.cardId;
            });
            if (!card || !canvas || !element) return;
            var bounds = canvas.getBoundingClientRect();
            var size = this._spReferenceSize(page);
            var output = rotatedSize(size, page.rotation);
            var dx = (event.clientX - pointer.startX) * output.width / Math.max(bounds.width, 1);
            var dy = (event.clientY - pointer.startY) * output.height / Math.max(bounds.height, 1);
            if (Math.abs(dx) + Math.abs(dy) < 2) return;
            pointer.moved = true;
            var displayRect = clone(pointer.startDisplayRect || rotateRect(pointer.startRect, size, page.rotation));
            if (pointer.mode === 'resize') {
                displayRect.w = clamp(snap(displayRect.w + dx, 8), 72, output.width - displayRect.x);
                displayRect.h = clamp(snap(displayRect.h + dy, 8), 56, output.height - displayRect.y);
            } else {
                displayRect.x = clamp(snap(displayRect.x + dx, 8), 0, output.width - displayRect.w);
                displayRect.y = clamp(snap(displayRect.y + dy, 8), 0, output.height - displayRect.h);
            }
            var logicalRect = unrotateRect(displayRect, size, page.rotation);
            card.rect = {
                x: Math.round(logicalRect.x), y: Math.round(logicalRect.y),
                w: Math.round(logicalRect.w), h: Math.round(logicalRect.h)
            };
            var aligning = this._spNearAlignment(page, card);
            element.classList.toggle('is-aligning', aligning);
            element.classList.toggle('is-invalid', this._spRectCollision(page, card));
            this._spApplyCardRect(element, card, page);
        },

        _spPointerUp(event) {
            var model = this._spEnsureModel();
            var pointer = model.pointer;
            model.pointer = null;
            document.getElementById('sp-editor-canvas')?.classList.remove('is-dragging');
            if (!pointer || (event.pointerId !== undefined && event.pointerId !== pointer.pointerId)) return;
            var page = this._spCurrentPage();
            var card = page.cards.find(function(item) { return item.id === pointer.cardId; });
            if (!pointer.moved || !card) { this._spRenderAll(); return; }
            if (this._spRectCollision(page, card)) {
                model.config = normalizeConfig(pointer.snapshot);
                this._spSetLayoutError('组件不能重叠，已恢复移动前的位置');
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
                if (model.histories[card.sourceRef] && !model.histories[card.sourceRef].loading) return;
                model.histories[card.sourceRef] = { loading: true, points: [] };
                apiGet('/api/periph-exec/history', { sourceRef: card.sourceRef }).then((response) => {
                    if (response && response.success && response.data) model.histories[card.sourceRef] = response.data;
                    else model.histories[card.sourceRef] = { points: [] };
                    if (this.currentPage === 'screen-panel') this._spRenderPreview();
                }).catch(() => { model.histories[card.sourceRef] = { points: [] }; });
            });
        },

        _spRefreshState(notify) {
            var model = this._spEnsureModel();
            if (notify) model.histories = {};
            var getter = typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
            return getter('/api/screen-panel/state').then((response) => {
                if (response && response.success && response.data) {
                    model.state = response.data;
                    var application = response.data.application || response.data.panel || {};
                    model.application = Object.assign({}, model.application || {}, application, {
                        revision: application.revision !== undefined ? application.revision : response.data.revision,
                        appliedRevision: response.data.appliedRevision
                    });
                    if (this.currentPage === 'screen-panel') {
                        this._spRenderPreview();
                        this._spRenderSurface(document.getElementById('sp-editor-canvas'), this._spCurrentPage(), true);
                        this._spApplyStatus();
                    }
                    if (notify) this._spSetMessage('显示数据和运行状态已刷新', 'success');
                } else throw new Error('设备状态暂不可用');
                return response;
            }).catch((error) => {
                if (model.state.clock) model.state.clock.quality = 'offline';
                if (this.currentPage === 'screen-panel' && !(error && error._pageAborted)) {
                    this._spRenderPreview();
                    this._spRenderSurface(document.getElementById('sp-editor-canvas'), this._spCurrentPage(), true);
                }
                if (notify && !(error && error._pageAborted)) this._spSetMessage('刷新失败，保留上次数据', 'error');
                return null;
            });
        },

        _spExportConfig() {
            var model = this._spEnsureModel();
            var envelope = {
                storageVersion: 1,
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
            var input = document.getElementById('sp-import-file');
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
                model.previewPageId = model.selectedPageId;
                model.selectedCardId = '';
                this._spMarkDirty();
                this._spSwitchTopTab('sp-preview');
                var validation = this._spValidateConfig(clean);
                this._spSetMessage(validation.errors.length
                    ? ('布局已导入为草稿，但有 ' + validation.errors.length + ' 项需要修正后才能保存：' + validation.errors[0])
                    : '布局已导入为草稿，请保存并应用', validation.errors.length ? 'error' : 'success');
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

        _spSave() {
            var model = this._spEnsureModel();
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
            var button = document.getElementById('sp-save-config');
            if (button) { button.disabled = true; button.textContent = '正在保存...'; }
            return apiPostJson('/api/screen-panel/config', {
                expectedRevision: model.revision,
                config: cleanConfig
            }).then((response) => {
                if (!response || response.success === false) throw new Error(response && response.message || '保存失败');
                var data = response.data || {};
                model.revision = finiteNumber(data.revision, model.revision + 1);
                model.config = normalizeConfig(data.config || model.config);
                model.savedConfig = clone(model.config);
                model.application = data.application || model.application;
                model.dirty = false; model.persisted = true;
                model.undo = [];
                model.redo = [];
                this._spClearDraft();
                this._spSetMessage(data.application && data.application.pendingRestart ? '已保存，重启后应用到物理屏' : '配置已保存', 'success');
                this._spRenderAll();
                return true;
            }).catch((error) => {
                if (error && error.status === 409) {
                    var currentRevision = error.data && (error.data.currentRevision || error.data.data && error.data.data.currentRevision);
                    this._spSetMessage('另一窗口已保存新配置' + (currentRevision !== undefined ? ('（版本 ' + currentRevision + '）') : '') + '。草稿已保留，请刷新核对。', 'error');
                } else if (!(error && error._pageAborted)) {
                    this._spSetMessage(error && error.data && error.data.message || error.message || '保存失败，请稍后重试', 'error');
                }
                return false;
            }).finally(function() {
                if (button) { button.disabled = false; button.textContent = '保存并应用'; }
            });
        },

        loadScreenPanelPage(options) {
            options = options || {};
            var model = this._spEnsureModel();
            this._spSetupEvents();
            this._spSetMessage('');
            var getter = options.noCache === true && typeof apiGetFresh === 'function' ? apiGetFresh : apiGet;
            var requests = [
                getter('/api/screen-panel/config'),
                this._spLoadAllChoices(getter),
                getter('/api/peripherals', { page: 1, pageSize: 50 }),
                getter('/api/screen-panel/state'),
                getter('/api/periph-exec/history/config'),
                getter('/api/screen-panel/weather')
            ];
            return Promise.allSettled(requests).then((results) => {
                var configResponse = results[0].status === 'fulfilled' ? results[0].value : null;
                if (!configResponse || configResponse.success === false || !configResponse.data) {
                    throw results[0].reason || new Error('设备屏幕配置不可用');
                }
                var payload = configResponse.data;
                var serverConfig = normalizeConfig(payload.config || null);
                model.revision = finiteNumber(payload.revision, 0);
                model.persisted = !!payload.config && model.revision > 0;
                model.capabilities = payload.capabilities || {};
                model.application = payload.application || {};
                model.savedConfig = clone(serverConfig);
                var restored = options.preserveDraft !== false ? this._spRestoreDraft(serverConfig, model.revision) : null;
                model.config = restored ? restored.config : serverConfig;
                model.dirty = !!restored && JSON.stringify(model.config) !== JSON.stringify(serverConfig);
                model.selectedPageId = restored && model.config.pages.some(function(page) { return page.id === restored.selectedPageId; }) ? restored.selectedPageId :
                    (model.config.settings.homePageId || model.config.pages[0].id);
                model.selectedCardId = restored ? restored.selectedCardId || '' : '';
                model.previewPageId = model.selectedPageId;
                var choicePayload = results[1].status === 'fulfilled' && results[1].value || {};
                model.choices = Array.isArray(choicePayload.items) ? choicePayload.items : [];
                model.choiceCapabilities = choicePayload.capabilities || {};
                var peripherals = results[2].status === 'fulfilled' ? unwrapList(results[2].value) : [];
                var capabilityDisplays = Array.isArray(model.capabilities.displays) ? model.capabilities.displays : [];
                model.displays = peripherals.filter(isDisplayPeripheral).concat(capabilityDisplays.filter(function(candidate) {
                    return !peripherals.some(function(item) { return String(item.id) === String(candidate.id || candidate.ref); });
                }));
                if (results[3].status === 'fulfilled' && results[3].value && results[3].value.success) {
                    model.state = results[3].value.data || { items: {} };
                    if (model.state.application || model.state.panel) {
                        model.application = Object.assign({}, model.application,
                            model.state.application || model.state.panel,
                            { appliedRevision: model.state.appliedRevision });
                    }
                }
                if (results[4].status === 'fulfilled' && results[4].value && results[4].value.success && results[4].value.data) {
                    model.historyConfig = results[4].value.data.config || null;
                    model.historyStatus = results[4].value.data.status || null;
                } else {
                    model.historyConfig = null;
                    model.historyStatus = null;
                }
                if (results[5].status === 'fulfilled' && results[5].value && results[5].value.success) {
                    if (!model.weatherDirty || options.preserveDraft === false) { model.weather = results[5].value.data; model.weatherDirty = false; this._spRenderWeather(); }
                } else { document.getElementById('sp-weather-status').textContent = '天气配置暂不可用'; }
                if (!model.config.displayRef && model.displays.length === 1) { model.config.displayRef = String(model.displays[0].id || model.displays[0].ref); model.dirty = true; }
                model.loaded = true;
                model.undo = [];
                model.redo = [];
                this._spRenderAll();
                this._spStartStatePolling();
                if (restored) this._spSetMessage('已恢复未保存草稿', 'success');
                return payload;
            }).catch((error) => {
                if (!(error && error._pageAborted)) {
                    this._spSetMessage('无法读取设备屏幕配置：' + (error && error.message || '设备暂不可用'), 'error');
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

    if (typeof AppState.onPageHide === 'function') {
        AppState.onPageHide('screen-panel', function() {
            if (this.currentPage !== 'screen-panel') return;
            this._spStopStatePolling();
            var model = this._spEnsureModel();
            if (model.dirty) this._spPersistDraftSoon();
        });
    }
    if (typeof AppState.onPageShow === 'function') {
        AppState.onPageShow('screen-panel', function() {
            if (this.currentPage === 'screen-panel' && this._spModel && this._spModel.loaded) this._spStartStatePolling();
        });
    }
})();
