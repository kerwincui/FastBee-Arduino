/** Pure screen-panel configuration and layout helpers; loaded before the editor. */
(function() {
    if (typeof AppState === 'undefined') return;

    var SOURCE_KINDS = ['data', 'status', 'gauge', 'bar', 'line', 'weather'];
    var SYSTEM_ACTION_KINDS = ['brightness', 'sleep', 'sleep-time', 'touch-beep', 'confirm-idle', 'buzzer', 'restart'];
    // The device preview reserves this logical height for page navigation.
    // Template cards must be laid out above it; otherwise the footer paints
    // over the last row and makes a valid-looking layout appear overlapped.
    var DEVICE_FOOTER_HEIGHT = 72;
    var MAX_BOTTOM_NAV_PAGES = 5;
    var MIN_VISIBLE_BRIGHTNESS_PERCENT = 10;
    var CARD_LABELS = {
        data: '数值',
        status: '状态',
        gauge: '仪表',
        bar: '柱状',
        line: '10点折线',
        control: '控制', motor: '电机控制', 'sleep-time': '自动息屏', 'touch-beep': '触摸音', 'confirm-idle': '操作确认',
        clock: '时钟',
        weather: '天气',
        'page-link': '页面入口',
        brightness: '设置亮度',
        sleep: '立即息屏',
        buzzer: '提示音',
        restart: '重启'
    };
    var CARD_DEFAULTS = {
        data: { title: '数值', display: 'value', size: 'small' },
        status: { title: '状态', display: 'status', size: 'small' },
        gauge: { title: '仪表', display: 'gauge', size: 'wide', min: 0, max: 100 },
        bar: { title: '柱状图', display: 'bar', size: 'wide', min: 0, max: 100 },
        line: { title: '最近趋势', display: 'recent10', size: 'wide' },
        control: { title: '控制', display: 'action', size: 'wide', confirm: true, min: 0, max: 100, step: 1, initialValue: 0 },
        clock: { title: '时间', display: 'clock', size: 'wide' },
        weather: { title: '天气', display: 'weather', size: 'large' },
        'page-link': { title: '打开页面', display: 'page-link', size: 'small' },
        motor: { title: '电机控制', display: 'motor', size: 'large', confirm: true, motorSpeed: 50, motorPulse: 1600, motorSpeedStep: 1, motorPulseStep: 100 },
        'sleep-time': { title: '自动息屏', display: 'sleep-time', size: 'wide', systemActionId: 'sleep-time' },
        'touch-beep': { title: '触摸音', display: 'touch-beep', size: 'wide', systemActionId: 'touch-beep' },
        'confirm-idle': { title: '操作确认', display: 'confirm-idle', size: 'wide', systemActionId: 'confirm-idle' },
        brightness: { title: '设置亮度', display: 'brightness', size: 'wide', systemActionId: 'brightness', actionValue: 70 },
        sleep: { title: '立即息屏', display: 'sleep', size: 'small', systemActionId: 'sleep' },
        buzzer: { title: '提示音', display: 'buzzer', size: 'small', systemActionId: 'buzzer' },
        restart: { title: '重启设备', display: 'restart', size: 'small', confirm: true, systemActionId: 'restart' }
    };
    var THEME_IDS = ['dark', 'light', 'high-contrast'];
    var WIRE_KINDS = {
        data: ['value', 'status', 'gauge', 'bar'],
        chart: ['line', 'bar'],
        control: ['switch', 'action', 'number', 'motor'],
        system: ['action', 'slider', 'select', 'switch'],
        clock: ['clock'],
        weather: ['weather'],
        'page-link': ['navigation']
    };

    function uiBackgroundPreset(value) {
        if (value === 'soft-gradient') return 'gradient';
        if (value === 'grid') return 'geometry';
        return value === 'gradient' || value === 'geometry' ? value : 'solid';
    }

    function wireBackgroundPreset(value) {
        if (value === 'gradient') return 'soft-gradient';
        if (value === 'geometry') return 'grid';
        return value === 'soft-gradient' || value === 'grid' ? value : 'solid';
    }

    function uiLayoutId(value) {
        return value === 'detail' ? 'data-detail' : (value || 'mixed-grid');
    }

    function wireLayoutId(value) {
        return value === 'data-detail' ? 'detail' : (value || 'mixed-grid');
    }

    function uiCardKind(card) {
        var kind = String(card && card.kind || 'data');
        var visual = String(card && (card.visualType || card.displayMode) || '');
        if (kind === 'control' && visual === 'motor') return 'motor';
        if (kind === 'chart') return visual === 'bar' ? 'bar' : 'line';
        if (kind === 'system') {
            var action = String(card && card.systemActionId || 'restart').toLowerCase();
            return SYSTEM_ACTION_KINDS.indexOf(action) >= 0 ? action : 'restart';
        }
        if (kind === 'data' && ['status', 'gauge', 'bar'].indexOf(visual) >= 0) return visual;
        if (CARD_DEFAULTS[kind]) return kind;
        return 'data';
    }

    function isSupportedCardInput(card) {
        if (!card || typeof card !== 'object') return false;
        if (CARD_DEFAULTS[card.kind]) return true;
        var visual = String(card.visualType || card.displayMode || '');
        return !!(WIRE_KINDS[card.kind] && WIRE_KINDS[card.kind].indexOf(visual) >= 0 &&
            (card.kind !== 'system' || SYSTEM_ACTION_KINDS.indexOf(String(card.systemActionId || '').toLowerCase()) >= 0));
    }

    function clone(value) {
        if (typeof structuredClone === 'function') return structuredClone(value);
        return JSON.parse(JSON.stringify(value));
    }

    function finiteNumber(value, fallback) {
        var number = Number(value);
        return Number.isFinite(number) ? number : fallback;
    }

    function clamp(value, min, max) {
        return Math.min(max, Math.max(min, value));
    }

    function snap(value, grid) {
        return Math.round(value / grid) * grid;
    }

    function normalizeBrightnessPercent(value) {
        var rounded = clamp(Math.round(finiteNumber(value, 70)), 0, 100);
        return rounded < MIN_VISIBLE_BRIGHTNESS_PERCENT ? MIN_VISIBLE_BRIGHTNESS_PERCENT : rounded;
    }

    function validIdentifier(value) {
        return /^[A-Za-z0-9_-]{1,40}$/.test(String(value || ''));
    }

    function normalizeBottomNavPageIds(settings, pages) {
        var pageIds = pages.map(function(page) { return String(page.id || ''); }).filter(validIdentifier);
        var requested = Array.isArray(settings && settings.bottomNavPageIds) ? settings.bottomNavPageIds.map(String) : [];
        var seen = {};
        var out = requested.filter(function(id) {
            if (!validIdentifier(id) || pageIds.indexOf(id) < 0 || seen[id]) return false;
            seen[id] = true; return true;
        }).slice(0, MAX_BOTTOM_NAV_PAGES);
        return out.length ? out : pageIds.slice(0, MAX_BOTTOM_NAV_PAGES);
    }

    function safeId(prefix) {
        var suffix = '';
        if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
            suffix = crypto.randomUUID().replace(/-/g, '').slice(0, 12);
        } else {
            suffix = Date.now().toString(36) + Math.random().toString(36).slice(2, 7);
        }
        return prefix + '-' + suffix;
    }

    function defaultConfig() {
        return {
            schemaVersion: 1,
            displayRef: null,
            settings: {
                brightnessPercent: 70,
                idleSleepSeconds: 120,
                touchBeepEnabled: true,
                controlConfirmIdleSeconds: 60,
                homePageId: 'home',
                themeId: 'dark',
                background: { presetId: 'solid' },
                allowSystemRestart: false,
                bottomNavPageIds: ['home']
            },
            pages: [{
                id: 'home',
                title: '首页',
                layoutId: 'mixed-grid',
                layoutMode: 'template',
                canvas: { width: 480, height: 480 },
                cards: []
            }]
        };
    }

    function normalizeConfig(input) {
        var fallback = defaultConfig();
        var config = input && typeof input === 'object' ? clone(input) : fallback;
        config.schemaVersion = 1;
        config.displayRef = config.displayRef || null;
        config.settings = Object.assign({}, fallback.settings, config.settings || {});
        delete config.settings.accentId;
        if (THEME_IDS.indexOf(String(config.settings.themeId || '')) < 0) config.settings.themeId = 'dark';
        config.settings.background = Object.assign({}, fallback.settings.background, config.settings.background || {});
        config.settings.background.presetId = uiBackgroundPreset(config.settings.background.presetId);
        if (!Array.isArray(config.pages) || config.pages.length === 0) config.pages = clone(fallback.pages);
        config.pages = config.pages.map(function(page, index) {
            var normalized = Object.assign({
                id: safeId('page'),
                title: '页面 ' + (index + 1),
                layoutId: 'mixed-grid',
                layoutMode: 'template',
                canvas: {},
                rotation: 0,
                cards: []
            }, page || {});
            normalized.id = String(normalized.id || safeId('page'));
            normalized.title = String(normalized.title || ('页面 ' + (index + 1))).slice(0, 64);
            normalized.layoutId = uiLayoutId(normalized.layoutId);
            if (normalized.themeId !== undefined && THEME_IDS.indexOf(String(normalized.themeId)) < 0) delete normalized.themeId;
            if (normalized.background && typeof normalized.background === 'object') {
                normalized.background = { presetId: uiBackgroundPreset(normalized.background.presetId) };
            }
            normalized.canvas = Object.assign({
                width: finiteNumber(normalized.logicalWidth, 480),
                height: finiteNumber(normalized.logicalHeight, 480)
            }, normalized.canvas || {});
            normalized.canvas.width = clamp(Math.round(finiteNumber(normalized.canvas.width, 480)), 128, 2048);
            normalized.canvas.height = clamp(Math.round(finiteNumber(normalized.canvas.height, 480)), 128, 2048);
            normalized.rotation = [0, 90, 180, 270].indexOf(Number(normalized.rotation)) >= 0 ? Number(normalized.rotation) : 0;
            normalized.cards = Array.isArray(normalized.cards) ? normalized.cards.map(function(card) {
                var kind = uiCardKind(card);
                var item = Object.assign({
                    id: safeId('card'), kind: kind, sourceRef: '', controlRef: '', targetPageId: '',
                    unit: '', precision: 1, min: 0, max: 100, step: 1, initialValue: 0,
                    confirm: false, actionValue: 70
                }, CARD_DEFAULTS[kind], card || {});
                item.id = String(item.id || safeId('card'));
                item.kind = kind;
                item.title = String(item.title || CARD_DEFAULTS[kind].title).slice(0, 64);
                item.unit = String(item.unit || '').slice(0, 16);
                item.precision = clamp(Math.round(finiteNumber(item.precision, 1)), 0, 6);
                item.display = String(card && (card.display || card.visualType || card.displayMode) || item.display || CARD_DEFAULTS[kind].display);
                if (kind === 'line') item.display = 'recent10';
                if (kind === 'page-link') item.display = 'page-link';
                if (SYSTEM_ACTION_KINDS.indexOf(kind) >= 0) {
                    item.systemActionId = kind;
                    item.display = kind;
                }
                if (kind === 'brightness') item.actionValue = normalizeBrightnessPercent(item.actionValue);
                item.targetPageId = String(item.targetPageId || item.pageId || '');
                item.confirm = kind === 'motor' || item.confirm === true || item.requireConfirmation === true;
                item.min = finiteNumber(item.min !== undefined ? item.min : item.rangeMin, 0);
                item.max = finiteNumber(item.max !== undefined ? item.max : item.rangeMax, 100);
                item.step = finiteNumber(item.step, 1);
                item.initialValue = finiteNumber(item.initialValue, item.min);
                item.sourceRefs = Array.isArray(item.sourceRefs) ? item.sourceRefs.map(String) : [];
                item.iconId = String(item.iconId || '').slice(0, 24);
                item.bindingFingerprint = String(item.bindingFingerprint || '').slice(0, 64);
                if (item.rect) {
                    item.rect = {
                        x: Math.round(finiteNumber(item.rect.x, 12)), y: Math.round(finiteNumber(item.rect.y, 12)),
                        w: Math.round(finiteNumber(item.rect.w, 210)), h: Math.round(finiteNumber(item.rect.h, 96))
                    };
                }
                return item;
            }) : [];
            return normalized;
        });
        config.settings.brightnessPercent = normalizeBrightnessPercent(config.settings.brightnessPercent);
        if (!config.pages.some(function(page) { return page.id === config.settings.homePageId; })) {
            config.settings.homePageId = config.pages[0].id;
        }
        config.settings.bottomNavPageIds = normalizeBottomNavPageIds(config.settings, config.pages);
        return config;
    }

    function sanitizeConfig(input, choices) {
        var config = normalizeConfig(input);
        return {
            schemaVersion: 1,
            displayRef: config.displayRef || null,
            settings: {
                brightnessPercent: normalizeBrightnessPercent(config.settings.brightnessPercent),
                idleSleepSeconds: finiteNumber(config.settings.idleSleepSeconds, 120),
                touchBeepEnabled: config.settings.touchBeepEnabled !== false,
                controlConfirmIdleSeconds: finiteNumber(config.settings.controlConfirmIdleSeconds, 60),
                homePageId: String(config.settings.homePageId || config.pages[0].id),
                themeId: String(config.settings.themeId || 'dark'),
                background: { presetId: wireBackgroundPreset(config.settings.background && config.settings.background.presetId) },
                allowSystemRestart: config.settings.allowSystemRestart === true,
                bottomNavPageIds: normalizeBottomNavPageIds(config.settings, config.pages)
            },
            pages: config.pages.map(function(page) {
                var cleanPage = {
                    id: String(page.id),
                    title: String(page.title).slice(0, 64),
                    layoutId: wireLayoutId(page.layoutId),
                    layoutMode: page.layoutMode === 'canvas' ? 'canvas' : 'template',
                    logicalWidth: clamp(Math.round(finiteNumber(page.canvas && page.canvas.width, 480)), 128, 2048),
                    logicalHeight: clamp(Math.round(finiteNumber(page.canvas && page.canvas.height, 480)), 128, 2048),
                    rotation: [0, 90, 180, 270].indexOf(Number(page.rotation)) >= 0 ? Number(page.rotation) : 0,
                    cards: page.cards.map(function(card) {
                        var wireKind = 'data';
                        var visualType = card.display || 'value';
                        if (card.kind === 'line') { wireKind = 'chart'; visualType = 'line'; }
                        else if (card.kind === 'motor') { wireKind = 'control'; visualType = 'motor'; }
                        else if (card.kind === 'control') {
                            wireKind = 'control';
                            if (visualType !== 'switch' && visualType !== 'action' && visualType !== 'number') {
                                var control = (choices || []).find(function(choice) { return choiceRef(choice) === card.controlRef; });
                                var controlType = String(control && control.type || '').toLowerCase();
                                visualType = controlType === 'switch' || controlType === 'number' ? controlType : 'action';
                            }
                        }
                        else if (card.kind === 'clock') { wireKind = 'clock'; visualType = 'clock'; }
                        else if (card.kind === 'weather') { wireKind = 'weather'; visualType = 'weather'; }
                        else if (card.kind === 'page-link') { wireKind = 'page-link'; visualType = 'navigation'; }
                        else if (SYSTEM_ACTION_KINDS.indexOf(card.kind) >= 0) { wireKind = 'system'; visualType = card.kind === 'brightness' ? 'slider' : card.kind === 'touch-beep' ? 'switch' : ['sleep-time', 'confirm-idle'].includes(card.kind) ? 'select' : 'action'; }
                        else if (card.kind === 'status') visualType = 'status';
                        else if (card.kind === 'gauge') visualType = 'gauge';
                        else if (card.kind === 'bar') visualType = 'bar';
                        else visualType = 'value';
                        var clean = {
                            id: String(card.id), kind: wireKind, visualType: visualType,
                            title: String(card.title || '').slice(0, 64),
                            precision: clamp(Math.round(finiteNumber(card.precision, 1)), 0, 6),
                            rect: {
                                x: Math.round(finiteNumber(card.rect && card.rect.x, 0)),
                                y: Math.round(finiteNumber(card.rect && card.rect.y, 0)),
                                w: Math.round(finiteNumber(card.rect && card.rect.w, 0)),
                                h: Math.round(finiteNumber(card.rect && card.rect.h, 0))
                            },
                            requireConfirmation: card.confirm === true,
                            historyMode: card.kind === 'line' ? 'recent10' : 'none'
                        };
                        if (card.unit) clean.unit = String(card.unit).slice(0, 16);
                        if (card.sourceRef) clean.sourceRef = String(card.sourceRef);
                        if (Array.isArray(card.sourceRefs) && card.sourceRefs.length) clean.sourceRefs = card.sourceRefs.slice(0, 4).map(String);
                        if (card.controlRef) clean.controlRef = String(card.controlRef);
                        if (card.kind === 'page-link') clean.pageId = String(card.targetPageId || '');
                        if (SYSTEM_ACTION_KINDS.indexOf(card.kind) >= 0) clean.systemActionId = card.kind;
                        if (card.kind === 'motor') ['motorSpeed', 'motorPulse', 'motorSpeedStep', 'motorPulseStep'].forEach(function(key) { clean[key] = finiteNumber(card[key], CARD_DEFAULTS.motor[key]); });
                        if (card.kind === 'brightness') clean.actionValue = clamp(Math.round(finiteNumber(card.actionValue, 70)), 0, 100);
                        if (card.iconId) clean.iconId = String(card.iconId).slice(0, 24);
                        if (card.bindingFingerprint) clean.bindingFingerprint = String(card.bindingFingerprint).slice(0, 64);
                        if ((['gauge', 'bar'].indexOf(card.kind) >= 0 ||
                             (card.kind === 'control' && visualType === 'number')) &&
                            finiteNumber(card.max, 100) > finiteNumber(card.min, 0)) {
                            clean.rangeMin = finiteNumber(card.min, 0);
                            clean.rangeMax = finiteNumber(card.max, 100);
                        }
                        if (card.kind === 'control' && visualType === 'number') {
                            clean.step = finiteNumber(card.step, 1);
                            clean.initialValue = finiteNumber(card.initialValue, clean.rangeMin);
                        }
                        return clean;
                    })
                };
                if (THEME_IDS.indexOf(String(page.themeId || '')) >= 0) cleanPage.themeId = String(page.themeId);
                if (page.background && page.background.presetId) {
                    cleanPage.background = { presetId: wireBackgroundPreset(page.background.presetId) };
                }
                return cleanPage;
            })
        };
    }


    function choiceRef(item) {
        return String(item && (item.ref || item.sourceRef || item.controlRef || item.id) || '');
    }

    function choiceLabel(item) {
        return String(item && (item.label || item.name || item.title || choiceRef(item)) || '未命名项目');
    }


    function isControlChoice(item) {
        var kind = String(item && (item.kind || item.category || item.choiceType || item.type) || '').toLowerCase();
        return kind === 'control' || kind === 'switch' || kind === 'action' || !!(item && item.controlRef);
    }

    function isWeatherChoice(item) {
        var kind = String(item && (item.kind || item.category || item.choiceType || item.type) || '').toLowerCase();
        return kind === 'weather' || !!(item && item.weather);
    }

    function unwrapList(response) {
        if (!response || response.success === false) return [];
        var data = response.data;
        if (Array.isArray(data)) return data;
        if (data && Array.isArray(data.items)) return data.items;
        if (data && Array.isArray(data.peripherals)) return data.peripherals;
        return [];
    }

    function displayStateMeta(application) {
        var panel = application && application.panel ? application.panel : application || {};
        var status = String(panel.status || panel.state || 'unknown').toLowerCase();
        var map = {
            running: ['运行中', 'status-badge-success'], active: ['运行中', 'status-badge-success'],
            applied: ['运行中', 'status-badge-success'], saved: ['已保存待应用', 'status-badge-warning'],
            draft: ['尚未持久化', 'status-badge-neutral'], apply_failed: ['应用失败', 'status-badge-danger'],
            disabled: ['未启用', 'status-badge-neutral'], pending_restart: ['待重启', 'status-badge-warning'],
            pending: ['待应用', 'status-badge-warning'], degraded: ['运行降级', 'status-badge-warning'],
            error: ['初始化失败', 'status-badge-danger'], no_display: ['未配置屏幕', 'status-badge-warning'],
            unavailable: ['不可用', 'status-badge-danger']
        };
        var meta = map[status] || ['状态未知', 'status-badge-neutral'];
        return {
            text: meta[0], className: meta[1], reason: panel.reason || panel.error || '',
            pendingRestart: panel.pendingRestart === true || panel.restartRequired === true
        };
    }

    function weatherIconMarkup(iconId, condition, escape) {
        var requested = String(iconId || '').toLowerCase();
        var normalized = requested || String(condition || '').toLowerCase();
        var glyph = '&#9675;';
        if (/clear-night|night|moon/.test(normalized)) glyph = '&#9790;';
        else if (/thunder|storm|lightning/.test(normalized)) glyph = '&#9889;';
        else if (normalized.indexOf('rain') >= 0 || /shower|雨/.test(normalized)) glyph = '&#9748;';
        else if (/snow|雪|cold/.test(normalized)) glyph = '&#10052;';
        else if (/clear|sun|sunny|晴|hot/.test(normalized)) glyph = '&#9728;';
        else if (/cloud|overcast|fog|阴|云|雾/.test(normalized)) glyph = '&#9729;';
        else if (/wind/.test(normalized)) glyph = '&#8776;';
        else if (requested) glyph = '&#9670;';
        return '<span class="sp-weather-icon" aria-hidden="true" data-icon-id="' +
            escape(requested || String(condition || 'unknown')) + '">' + glyph + '</span>';
    }

    function cardIconMarkup(iconId, escape) {
        var id = String(iconId || '').toLowerCase();
        if (!id) return '';
        var glyphs = {
            home: '&#8962;', data: '123', chart: '&#9585;', switch: '&#9679;', action: '&#9654;',
            scene: '&#10022;', clock: '&#9716;', weather: '&#9728;', link: '&#8594;', brightness: '&#9728;',
            sleep: '&#9680;', buzzer: '&#9834;', restart: '&#8635;',
            info: 'i', warning: '!'
        };
        return '<span class="sp-card-icon" aria-hidden="true" data-icon-id="' + escape(id) + '">' +
            (glyphs[id] || '&#9670;') + '</span>';
    }

    function buildThreeScreenTemplate(currentConfig, choices, size, rotation) {
        var available = (choices || []).filter(function(choice) { return choice && choice.available !== false; });
        var weather = available.find(isWeatherChoice);
        var numeric = available.filter(function(choice) { return isNumericChoice(choice) && !choiceRef(choice).startsWith('system.') && !choiceRef(choice).startsWith('weather.'); });
        var climate = numeric.filter(function(choice) { return /温|湿|temperature|humidity/i.test(choiceLabel(choice) + ' ' + choiceRef(choice)); });
        numeric = (climate.length ? climate : numeric).slice(0, 2);
        var motor = available.find(function(choice) { return choice.type === 'motor'; });
        function card(kind, id, choice) { return Object.assign(clone(CARD_DEFAULTS[kind]), { id: id, kind: kind }, choice ? {
            title: choiceLabel(choice), sourceRef: kind === 'motor' ? '' : choiceRef(choice),
            controlRef: kind === 'motor' ? choiceRef(choice) : '', unit: choice.unit || '', precision: finiteNumber(choice.precision, 1)
        } : {}); }
        function page(id, title, cards) { return { id: id, title: title, layoutId: 'mixed-grid', layoutMode: 'template', rotation: rotation, canvas: clone(size), cards: cards }; }
        var home = [card('clock', 'home-clock')].concat(numeric.map(function(item, i) { return card('data', 'home-data-' + i, item); }));
        if (weather) home.push(card('weather', 'home-weather', weather));
        var pages = [page('home', '首页', home)];
        if (numeric.length) pages.push(page('trend', '温湿度趋势', numeric.map(function(item, i) { return card('line', 'trend-' + i, item); })));
        if (motor) pages.push(page('motor', '电机控制', [card('motor', 'motor-main', motor)]));
        var system = page('system', '系统设置', ['brightness', 'sleep-time', 'touch-beep', 'confirm-idle', 'sleep'].map(function(kind) { return card(kind, 'system-' + kind); }));
        system.layoutId = 'data-detail';
        pages.push(system);
        var config = normalizeConfig({ schemaVersion: 1, displayRef: currentConfig.displayRef || null, settings: clone(currentConfig.settings), pages: pages });
        config.settings.homePageId = 'home'; return config;
    }

    function buildPageTemplate(template, currentConfig, choices, size, rotation, id) {
        var starter = buildThreeScreenTemplate(currentConfig, choices, size, rotation);
        var sourceId = { weather: 'home', data: 'home', trend: 'trend', motor: 'motor', system: 'system' }[template];
        var page = clone(starter.pages.find(function(item) { return item.id === sourceId; }) || {
            title: template === 'motor' ? '电机控制' : '自定义页面', layoutId: 'mixed-grid', layoutMode: 'template', rotation: rotation, canvas: clone(size), cards: []
        });
        if (template === 'control' || template === 'execute') {
            page.title = template === 'execute' ? '快捷执行' : '已绑定控制';
            page.cards = (choices || []).filter(function(choice) { return isControlChoice(choice) && choice.type !== 'motor' && choice.available !== false && choice.allowPanelControl !== false && (template !== 'execute' || choice.type === 'action'); }).slice(0, 3).map(function(choice) {
                return Object.assign(clone(CARD_DEFAULTS.control), { kind: 'control', title: choiceLabel(choice), controlRef: choiceRef(choice), display: choice.type || 'action' });
            });
        }
        page.id = id;
        page.cards.forEach(function(card, index) { card.id = id + '-card-' + index; }); return page;
    }

    function rectsOverlap(left, right) {
        return left.x < right.x + right.w && left.x + left.w > right.x &&
            left.y < right.y + right.h && left.y + left.h > right.y;
    }

    function rotationDegrees(value) {
        var rotation = finiteNumber(value, 0);
        if (rotation >= 0 && rotation <= 3) rotation *= 90;
        return [0, 90, 180, 270].indexOf(rotation) >= 0 ? rotation : 0;
    }

    function rotatedSize(size, rotation) {
        rotation = rotationDegrees(rotation);
        return rotation === 90 || rotation === 270
            ? { width: size.height, height: size.width }
            : { width: size.width, height: size.height };
    }

    // Page/card coordinates always stay in the unrotated logical coordinate space.
    function rotateRect(rect, size, rotation) {
        rotation = rotationDegrees(rotation);
        var source = {
            x: finiteNumber(rect && rect.x, 0),
            y: finiteNumber(rect && rect.y, 0),
            w: finiteNumber(rect && rect.w, 0),
            h: finiteNumber(rect && rect.h, 0)
        };
        if (rotation === 90) {
            return { x: size.height - source.y - source.h, y: source.x, w: source.h, h: source.w };
        }
        if (rotation === 180) {
            return { x: size.width - source.x - source.w, y: size.height - source.y - source.h, w: source.w, h: source.h };
        }
        if (rotation === 270) {
            return { x: source.y, y: size.width - source.x - source.w, w: source.h, h: source.w };
        }
        return source;
    }

    function unrotateRect(rect, size, rotation) {
        rotation = rotationDegrees(rotation);
        return rotateRect(rect, rotatedSize(size, rotation), (360 - rotation) % 360);
    }


    function referenceSize(page, config, context) {
        var model = context._spEnsureModel();
        config = config || model.config;
        var pageWidth = finiteNumber(page && page.canvas && page.canvas.width, 0);
        var pageHeight = finiteNumber(page && page.canvas && page.canvas.height, 0);
        if (pageWidth > 0 && pageHeight > 0) {
            return { width: clamp(pageWidth, 128, 2048), height: clamp(pageHeight, 128, 2048) };
        }
        var selectedDisplay = model.displays.find(function(item) {
            return String(item.id || item.ref || '') === String(config.displayRef || '');
        });
        var params = selectedDisplay && (selectedDisplay.params || selectedDisplay.config || selectedDisplay);
        var width = finiteNumber(params && params.width, page && page.canvas && page.canvas.width || 480);
        var height = finiteNumber(params && params.height, page && page.canvas && page.canvas.height || 480);
        var rotation = finiteNumber(params && params.rotation, 0);
        if (rotation === 1 || rotation === 3 || rotation === 90 || rotation === 270) {
            var swap = width; width = height; height = swap;
        }
        return { width: clamp(width, 128, 2048), height: clamp(height, 128, 2048) };
    }

    function templateMetrics(page, config, context) {
        var size = context._spReferenceSize(page, config);
        var gap = Math.max(8, Math.round(size.width / 48));
        var template = page.layoutId || 'mixed-grid';
        var cols = size.width >= 720 ? 3 : 2;
        var contentHeight = Math.max(64, size.height - DEVICE_FOOTER_HEIGHT);
        var rowHeight = Math.max(66, Math.round(contentHeight / 4.7));
        if (template === 'data-detail') { cols = 1; rowHeight = Math.max(58, Math.round(contentHeight / 6)); }
        if (template === 'tile-grid') { cols = size.width >= 640 ? 3 : 2; rowHeight = Math.max(72, Math.round(contentHeight / 4.2)); }
        if (template === 'info-home') { cols = 2; rowHeight = Math.max(72, Math.round(contentHeight / 4)); }
        return { width: size.width, height: size.height, contentHeight: contentHeight, gap: gap, cols: cols, rowHeight: rowHeight };
    }

    function layoutTemplatePage(page, config, context) {
        var metrics = templateMetrics(page, config, context);
        if (page.cards.length === 1 && page.cards[0].kind === 'motor') {
            page.cards[0].rect = { x: 12, y: 8, w: Math.max(1, metrics.width - 24), h: Math.max(1, metrics.contentHeight - 16) }; return;
        }
        if (page.layoutId === 'data-detail' && page.cards.length === 5 && page.cards.every(function(card) { return SYSTEM_ACTION_KINDS.includes(card.kind); })) {
            var detailGap = Math.min(metrics.gap, 8);
            var detailX = Math.max(detailGap, Math.round(metrics.width / 40));
            var detailColumns = Math.floor((metrics.contentHeight - detailGap * 6) / 5) < 44 ? 2 : 1;
            var detailRows = Math.ceil(page.cards.length / detailColumns);
            var detailWidth = Math.max(1, Math.floor((metrics.width - detailX * 2 - detailGap * (detailColumns - 1)) / detailColumns));
            var detailHeight = Math.max(44, Math.floor((metrics.contentHeight - detailGap * (detailRows + 1)) / detailRows));
            page.cards.forEach(function(card, index) {
                var column = index % detailColumns;
                var row = Math.floor(index / detailColumns);
                card.rect = {
                    x: detailX + column * (detailWidth + detailGap),
                    y: detailGap + row * (detailHeight + detailGap),
                    w: detailWidth, h: detailHeight
                };
            });
            return;
        }
        var occupied = [];
        var placements = [];
        var hasMotor = page.cards.some(function(card) { return card.kind === 'motor'; });
        function canPlace(row, col, colSpan, rowSpan) {
            for (var y = row; y < row + rowSpan; y++) {
                for (var x = col; x < col + colSpan; x++) {
                    if (occupied[y] && occupied[y][x]) return false;
                }
            }
            return col + colSpan <= metrics.cols;
        }
        function place(row, col, colSpan, rowSpan) {
            for (var y = row; y < row + rowSpan; y++) {
                if (!occupied[y]) occupied[y] = [];
                for (var x = col; x < col + colSpan; x++) occupied[y][x] = true;
            }
        }
        page.cards.forEach(function(card, index) {
            var size = card.size || 'small';
            var colSpan = size === 'small' ? 1 : Math.min(2, metrics.cols);
            var rowSpan = size === 'large' ? 2 : 1;
            // Keep controls beside a motor when possible. This leaves one
            // full-height motor slot instead of shrinking the motor below its
            // 320px minimum just because two wide controls came first.
            if (hasMotor && card.kind === 'control' && metrics.cols > 1 && size !== 'small') colSpan = 1;
            if (page.layoutId === 'data-detail') { colSpan = 1; rowSpan = 1; }
            if (page.layoutId === 'info-home' && index === 0) { colSpan = metrics.cols; rowSpan = 2; }
            if (card.kind === 'bar' && card.sourceRefs && card.sourceRefs.length) {
                rowSpan = Math.max(rowSpan, Math.ceil((36 + 40 * card.sourceRefs.length + metrics.gap) /
                    (metrics.rowHeight + metrics.gap)));
            }
            if (card.kind === 'control' && card.display === 'number' && metrics.rowHeight < 112) {
                rowSpan = Math.max(rowSpan, 2);
            }
            if (card.kind === 'motor') {
                colSpan = metrics.cols;
                rowSpan = Math.max(hasMotor && page.cards.length > 1 ? 5 : 1,
                    Math.ceil((320 + metrics.gap) / (metrics.rowHeight + metrics.gap)));
            }
            if (card.kind === 'line') { colSpan = metrics.cols; rowSpan = Math.max(2, rowSpan); }
            var row = 0;
            var col = 0;
            var found = false;
            while (!found && row < 100) {
                for (col = 0; col < metrics.cols; col++) {
                    if (canPlace(row, col, colSpan, rowSpan)) { found = true; break; }
                }
                if (!found) row++;
            }
            place(row, col, colSpan, rowSpan);
            placements.push({ card: card, row: row, col: col, colSpan: colSpan, rowSpan: rowSpan });
        });

        // The number of rows is data-dependent. Fit the completed grid into
        // the usable area before assigning pixels, so adding a card cannot
        // push later cards past the footer or make them overlap visually.
        var usedRows = placements.reduce(function(max, item) { return Math.max(max, item.row + item.rowSpan); }, 0);
        if (!usedRows) return;
        var layoutGap = metrics.gap;
        var fittedRowHeight = Math.floor((metrics.contentHeight - layoutGap * (usedRows + 1)) / usedRows);
        if (fittedRowHeight < 44) {
            layoutGap = Math.max(2, Math.min(layoutGap,
                Math.floor((metrics.contentHeight - 44 * usedRows) / (usedRows + 1))));
            fittedRowHeight = Math.floor((metrics.contentHeight - layoutGap * (usedRows + 1)) / usedRows);
        }
        fittedRowHeight = Math.max(1, fittedRowHeight);
        var colWidth = (metrics.width - layoutGap * (metrics.cols + 1)) / metrics.cols;
        placements.forEach(function(item) {
            item.card.rect = {
                x: Math.round(layoutGap + item.col * (colWidth + layoutGap)),
                y: Math.round(layoutGap + item.row * (fittedRowHeight + layoutGap)),
                w: Math.round(colWidth * item.colSpan + layoutGap * (item.colSpan - 1)),
                h: Math.round(fittedRowHeight * item.rowSpan + layoutGap * (item.rowSpan - 1))
            };
        });
    }

    function templateRectIsUsable(rect, size, contentHeight) {
        return !!rect && [rect.x, rect.y, rect.w, rect.h].every(Number.isInteger) &&
            rect.x >= 0 && rect.y >= 0 && rect.w > 0 && rect.h > 0 &&
            rect.x + rect.w <= size.width && rect.y + rect.h <= contentHeight;
    }

    function templatePageNeedsLayout(page, config, context) {
        if (!page || page.layoutMode !== 'template') return false;
        var size = context._spReferenceSize(page, config);
        var contentHeight = Math.max(64, size.height - DEVICE_FOOTER_HEIGHT);
        for (var index = 0; index < page.cards.length; index++) {
            if (!templateRectIsUsable(page.cards[index].rect, size, contentHeight)) return true;
            for (var compare = index + 1; compare < page.cards.length; compare++) {
                if (!templateRectIsUsable(page.cards[compare].rect, size, contentHeight) ||
                    rectsOverlap(page.cards[index].rect, page.cards[compare].rect)) return true;
            }
        }
        return false;
    }

    function ensurePageRects(page, config, context) {
        if (!page) return;
        if (page.layoutMode === 'template' && templatePageNeedsLayout(page, config, context)) {
            context._spLayoutTemplatePage(page, config);
        }
    }

    function validateConfig(config, context) {
        var limits = context._spLimits();
        var model = context._spEnsureModel();
        var errors = [];
        var cardCount = 0;
        if (!Number.isInteger(config.settings.controlConfirmIdleSeconds) || config.settings.controlConfirmIdleSeconds < 15 || config.settings.controlConfirmIdleSeconds > 600) errors.push('操作确认间隔须为 15 到 600 秒');
        if (!Array.isArray(config.pages) || config.pages.length === 0) errors.push('至少保留一个页面');
        if (config.pages.length > limits.maxPages) errors.push('页面数量超过设备上限 ' + limits.maxPages);
        var pageIds = {};
        var cardIds = {};
        if (config.displayRef && !model.displays.some(function(display) {
            return String(display.id || display.ref || '') === String(config.displayRef);
        })) errors.push('显示设备引用已不可用');
        (config.settings.bottomNavPageIds || []).forEach(function(id) {
            if (!validIdentifier(id) || !config.pages.some(function(page) { return page.id === id; })) errors.push('???????????' + id);
        });
        config.pages.forEach((page) => {
            if (!/^[A-Za-z0-9_-]{1,40}$/.test(String(page.id || ''))) errors.push('页面 ID 格式无效：' + page.id);
            if (pageIds[page.id]) errors.push('页面 ID 重复：' + page.id);
            pageIds[page.id] = true;
            cardCount += page.cards.length;
            if (page.cards.length > limits.maxCardsPerPage) errors.push('“' + page.title + '”组件数量超过上限 ' + limits.maxCardsPerPage);
            context._spEnsurePageRects(page, config);
            var size = context._spReferenceSize(page, config);
            page.cards.forEach((card, index) => {
                var label = '“' + page.title + ' / ' + (card.title || CARD_LABELS[card.kind]) + '”';
                var rect = card.rect || {};
                var interactive = ['control', 'motor', 'page-link'].concat(SYSTEM_ACTION_KINDS).indexOf(card.kind) >= 0;
                if (![rect.x, rect.y, rect.w, rect.h].every(Number.isInteger) || rect.x < 0 || rect.y < 0 || rect.w < 1 || rect.h < 1 ||
                    rect.x + rect.w > size.width || rect.y + rect.h > size.height || (interactive && (rect.w < 44 || rect.h < 44))) {
                    errors.push(label + '超出画布或尺寸过小');
                }
                if (!/^[A-Za-z0-9_-]{1,40}$/.test(String(card.id || ''))) errors.push(label + '的组件 ID 格式无效');
                if (cardIds[card.id]) errors.push('组件 ID 重复：' + card.id);
                cardIds[card.id] = true;
                if (SOURCE_KINDS.indexOf(card.kind) >= 0 && card.kind !== 'weather' && !card.sourceRef) errors.push(label + '未选择来源');
                if (card.sourceRef && !model.choices.some(function(choice) { return choiceRef(choice) === card.sourceRef && !isControlChoice(choice); })) {
                    errors.push(label + '的数据来源已不可用');
                }
                if (Array.isArray(card.sourceRefs) && card.sourceRefs.length) {
                    if (card.kind !== 'bar' || card.sourceRefs.length > 4 ||
                        card.sourceRefs[0] !== card.sourceRef || new Set(card.sourceRefs).size !== card.sourceRefs.length) {
                        errors.push(label + '最多比较四个不重复的柱状来源，第一项须为数据来源');
                    }
                    var seriesChoices = card.sourceRefs.map(function(ref) {
                        return model.choices.find(function(choice) { return choiceRef(choice) === ref; });
                    });
                    if (seriesChoices.some(function(choice) { return !isNumericChoice(choice); }) ||
                        seriesChoices.some(function(choice) { return String(choice && choice.unit || '') !== String(seriesChoices[0] && seriesChoices[0].unit || ''); })) {
                        errors.push(label + '的对比来源必须是同单位数值');
                    }
                    if (rect.w < 100 || rect.h < 36 + 40 * card.sourceRefs.length) {
                        errors.push(label + '请增大柱状卡片：至少宽 100、高 ' + (36 + 40 * card.sourceRefs.length));
                    }
                }
                if (card.kind === 'motor') {
                    if (!model.choices.some(function(choice) { return choice.type === 'motor' && choiceRef(choice) === card.controlRef; })) errors.push(label + '请选择电机');
                    if (rect.w < 280 || rect.h < 320) errors.push(label + '电机组件至少需要 280 × 320');
                    ['motorSpeed', 'motorPulse', 'motorSpeedStep', 'motorPulseStep'].forEach(function(key) {
                        var value = card[key]; if (!Number.isInteger(value) || value < (key.endsWith('Step') ? 1 : 0) || value > 65535) errors.push(label + '电机参数无效');
                    });
                }
                if (['gauge', 'bar'].indexOf(card.kind) >= 0 && !(Number(card.max) > Number(card.min))) {
                    errors.push(label + '量程上限必须大于下限');
                }
                if (card.kind === 'control' && card.controlRef && !model.choices.some(function(choice) {
                    return choiceRef(choice) === card.controlRef && isControlChoice(choice) &&
                        choice.available !== false && choice.allowPanelControl !== false;
                })) errors.push(label + '的控制项已不可用');
                if (card.kind === 'control' && card.display === 'number') {
                    var numericValues = [card.min, card.max, card.step, card.initialValue].map(Number);
                    var precision = Number(card.precision);
                    var scale = Math.pow(10, precision);
                    var aligned = function(value) {
                        return Number.isFinite(value * scale) &&
                            Math.abs(value * scale - Math.round(value * scale)) <= 1e-7 * Math.max(1, Math.abs(value * scale));
                    };
                    var positions = (numericValues[3] - numericValues[0]) / numericValues[2];
                    if (!numericValues.every(Number.isFinite) || !Number.isInteger(precision) || precision < 0 || precision > 6 ||
                        !(numericValues[1] > numericValues[0]) || !(numericValues[2] > 0) ||
                        numericValues[2] > numericValues[1] - numericValues[0] ||
                        numericValues[3] < numericValues[0] || numericValues[3] > numericValues[1] ||
                        !numericValues.every(aligned) || !Number.isFinite(positions) ||
                        Math.abs(positions - Math.round(positions)) > 1e-7 * Math.max(1, Math.abs(positions))) {
                        errors.push(label + '的数值范围、步长、初始值或小数位无效');
                    }
                    if (rect.w < 180 || rect.h < 112) errors.push(label + '请增大数值控件：至少宽 180、高 112');
                    var numericChoice = model.choices.find(function(choice) { return choiceRef(choice) === card.controlRef; });
                    if (numericChoice && String(numericChoice.type || '').toLowerCase() !== 'number') {
                        errors.push(label + '绑定的规则不是数值控制');
                    }
                }
                if (card.kind === 'page-link' && !pageIds[card.targetPageId] && !config.pages.some(function(item) { return item.id === card.targetPageId; })) errors.push(label + '未选择有效目标页');
                if (card.kind === 'brightness' && !(Number(card.actionValue) >= 0 && Number(card.actionValue) <= 100)) errors.push(label + '的目标亮度须为 0 到 100（0 为最低亮度）');
                if (card.kind === 'restart' && config.settings.allowSystemRestart !== true) errors.push(label + '需要先开启重启许可');
                if (card.kind === 'restart' && card.confirm !== true) errors.push(label + '必须开启执行前二次确认');
                for (var compare = index + 1; compare < page.cards.length; compare++) {
                    if (rectsOverlap(rect, page.cards[compare].rect || {})) errors.push(label + '与“' + (page.cards[compare].title || CARD_LABELS[page.cards[compare].kind]) + '”重叠');
                }
            });
        });
        if (cardCount > limits.maxCards) errors.push('组件总数超过设备上限 ' + limits.maxCards);
        return { errors: Array.from(new Set(errors)), cardCount: cardCount };
    }

    function isNumericChoice(choice) {
        return !!choice && !isControlChoice(choice) && !isWeatherChoice(choice) &&
            String(choice.type || choice.valueType || '').toLowerCase() === 'number';
    }

    var helpers = {
        SOURCE_KINDS,
        SYSTEM_ACTION_KINDS,
        CARD_LABELS,
        CARD_DEFAULTS,
        isSupportedCardInput,
        clone,
        finiteNumber,
        clamp,
        snap,
        normalizeBrightnessPercent,
        normalizeBottomNavPageIds,
        safeId,
        defaultConfig,
        normalizeConfig,
        sanitizeConfig,
        referenceSize,
        templateMetrics,
        layoutTemplatePage,
        ensurePageRects,
        validateConfig,
        isNumericChoice,
        choiceRef,
        choiceLabel,
        unwrapList,
        isControlChoice,
        isWeatherChoice,
        buildThreeScreenTemplate,
        buildPageTemplate,
        rectsOverlap,
        displayStateMeta,
        weatherIconMarkup,
        cardIconMarkup,
        rotationDegrees,
        rotatedSize,
        rotateRect,
        unrotateRect,
    };
    if (typeof AppState.registerModule === 'function') {
        AppState.registerModule('screen-panel-model', { _screenPanelModelHelpers: helpers });
    } else {
        AppState._screenPanelModelHelpers = helpers;
    }
})();

// Settings share the existing lazy model asset; no additional network request.
(function() {
    if (typeof AppState === "undefined") return;
    var helpers = AppState._screenPanelModelHelpers;
    var SOURCE_KINDS = helpers.SOURCE_KINDS, choiceRef = helpers.choiceRef, choiceLabel = helpers.choiceLabel;
    var isControlChoice = helpers.isControlChoice, isWeatherChoice = helpers.isWeatherChoice, finiteNumber = helpers.finiteNumber;
    var CARD_LABELS = helpers.CARD_LABELS, SYSTEM_ACTION_KINDS = helpers.SYSTEM_ACTION_KINDS;
    var clamp = helpers.clamp, cardIconMarkup = helpers.cardIconMarkup, weatherIconMarkup = helpers.weatherIconMarkup;
    AppState._screenPanelSettingsMethods = {
        _spCardContent(card, config) {
            var state = this._spCardState(card);
            var value = state.value !== undefined ? state.value : state.text;
            var unit = card.unit || state.unit || '';
            var title = escapeHtml(card.title || CARD_LABELS[card.kind] || '组件');
            var quality = String(state.quality || '').toLowerCase();
            var meta = ({ stale: '数据已过期', offline: '离线', error: '读取失败' })[quality] || '';
            var titleIcon = card.kind === 'weather' ? '' : cardIconMarkup(card.iconId, escapeHtml);
            var content = '<div class="sp-card-title">' + titleIcon + '<span>' + title + '</span></div>';
            if (card.kind === 'data') {
                content += '<div class="sp-card-value-row"><span class="sp-card-value">' + escapeHtml(this._spFormatValue(value, card.precision)) + '</span>' +
                    (unit ? '<span class="sp-card-unit">' + escapeHtml(unit) + '</span>' : '') + '</div>';
            } else if (card.kind === 'status') {
                var status = String(state.state !== undefined ? state.state : value || 'unknown').toLowerCase();
                var dot = status === 'on' || status === 'true' || status === '1' ? 'is-on' :
                    (status === 'off' || status === 'false' || status === '0' ? 'is-off' : 'is-pending');
                content += '<div class="sp-status-value"><span class="sp-status-dot ' + dot + '"></span><span class="sp-card-value">' +
                    escapeHtml(state.text || (dot === 'is-on' ? '开启' : dot === 'is-off' ? '关闭' : '未知')) + '</span></div>';
            } else if (card.kind === 'bar' && card.sourceRefs && card.sourceRefs.length) {
                content += this._spBarSeriesContent(card, state);
            } else if (card.kind === 'gauge' || card.kind === 'bar') {
                var numeric = Number(value);
                var min = finiteNumber(card.min, 0);
                var max = finiteNumber(card.max, 100);
                var percent = Number.isFinite(numeric) && max > min ? clamp((numeric - min) * 100 / (max - min), 0, 100) : 0;
                content += '<div class="sp-card-value-row"><span class="sp-card-value">' + escapeHtml(this._spFormatValue(value, card.precision)) + '</span>' +
                    (unit ? '<span class="sp-card-unit">' + escapeHtml(unit) + '</span>' : '') + '</div>' +
                    '<div class="' + (card.kind === 'gauge' ? 'sp-gauge-track' : 'sp-bar-track') + '"><div class="' +
                    (card.kind === 'gauge' ? 'sp-gauge-fill' : 'sp-bar-fill') + '" style="--sp-progress:' + percent.toFixed(1) + '%"></div></div>' +
                    '<div class="sp-gauge-labels"><span>' + escapeHtml(String(min)) + '</span><span>' + escapeHtml(String(max)) + '</span></div>';
            } else if (card.kind === 'line') {
                content += this._spLineSvg(card) + '<div class="sp-card-meta">最近 10 条有效采样</div>';
            } else if (card.kind === 'motor') {
                var motor = this._spEnsureModel().choices.find(function(choice) { return choiceRef(choice) === card.controlRef; });
                var precision = state.motorPrecision || motor && motor.precision || 0;
                content += this._spMotorPreview(card, state, precision);
            } else if (card.kind === 'control') {
                if (card.display === 'number') {
                    var numericValue = finiteNumber(card.initialValue, finiteNumber(card.min, 0));
                    content += '<div class="sp-number-control-preview" aria-label="数值控制预览">' +
                        '<button type="button" disabled aria-disabled="true">-</button>' +
                        '<span><strong>' + escapeHtml(this._spFormatValue(numericValue, card.precision)) + '</strong>' +
                        (card.unit ? '<small>' + escapeHtml(card.unit) + '</small>' : '') + '</span>' +
                        '<button type="button" disabled aria-disabled="true">+</button></div>' +
                        '<button class="sp-number-execute" type="button" disabled aria-disabled="true">执行</button>';
                } else {
                    var controlText = !card.controlRef ? '待配置操作' : state.pending ? '等待回读' : (state.text || (card.display === 'action' ? '执行一次' : state.state === true || state.state === 'on' ? '开启' : state.state === false || state.state === 'off' ? '关闭' : '状态未知'));
                    content += '<div class="sp-control-preview"><span class="sp-card-value">' + escapeHtml(controlText) +
                        '</span><button type="button" disabled aria-disabled="true">仅预览</button></div>';
                }
            } else if (card.kind === 'clock') {
                var clock = this._spEnsureModel().state.clock || {};
                var validClock = clock.valid === true && /^\d{2}:\d{2}$/.test(String(clock.time || '')) && /^\d{4}-\d{2}-\d{2}$/.test(String(clock.date || ''));
                content += '<div class="sp-card-value sp-clock-value">' + escapeHtml(validClock ? clock.time : '时间未同步') + '</div>' +
                    '<div class="sp-card-meta">' + escapeHtml(clock.quality === 'offline' ? '设备时间未更新' : validClock ? clock.date : '等待设备校时') + '</div>';
            } else if (card.kind === 'weather') {
                var condition = String(state.condition || state.conditionCode || '').toLowerCase();
                var temperature = state.temperature !== undefined ? state.temperature : value;
                var weatherMeta = !card.sourceRef ? '待配置天气来源' : state.description || state.city || (temperature === undefined ? '暂无天气数据' : '天气来源');
                content += '<div class="sp-weather-main">' + weatherIconMarkup(card.iconId, condition, escapeHtml) +
                    '<div class="sp-weather-copy"><div class="sp-card-value">' + escapeHtml(state.text || this._spFormatValue(temperature, card.precision)) +
                    (unit ? ' ' + escapeHtml(unit) : '') + '</div><div class="sp-card-meta">' + escapeHtml(weatherMeta) + '</div></div></div>' +
                    '';
            } else if (card.kind === 'page-link') {
                var target = (config || this._spEnsureModel().config).pages.find(function(page) { return page.id === card.targetPageId; });
                content += '<div class="sp-page-link-preview"><button type="button" disabled aria-disabled="true">' + escapeHtml(target ? target.title : '目标页不可用') + '</button></div>';
            } else if (SYSTEM_ACTION_KINDS.indexOf(card.kind) >= 0) {
                content += this._spSystemPreview(card, (config || this._spEnsureModel().config).settings);
            }
            if (state.error) meta = state.error;
            if (meta) content += '<div class="sp-card-meta">' + escapeHtml(meta) + '</div>';
            return content;
        },


        _spMotorPreview(card, state, precision) {
            var values = [['速度', this._spFormatValue(card.motorSpeed / Math.pow(10, precision), precision)], ['脉冲', card.motorPulse]];
            return '<div class="sp-motor-preview"><small>' + (state.pending ? '指令处理中' : '运行状态未确认') + '</small>' + values.map(function(item) {
                return '<div class="sp-motor-param"><span><small>' + item[0] + '</small><strong>' + escapeHtml(String(item[1])) + '</strong></span><button disabled>−</button><button disabled>＋</button><button disabled>应用</button></div>';
            }).join('') + '<div class="sp-motor-actions"><button disabled>正转</button><button disabled>反转</button></div><button class="sp-motor-stop" disabled>停止</button></div>';
        },
        _spSystemPreview(card, settings) {
            var kind = card.kind, value;
            if (kind === 'brightness') {
                value = finiteNumber(settings.brightnessPercent, 70);
                return '<div class="sp-system-preview"><output>' + (value ? value + '%' : '最低亮度') + '</output><input type="range" min="0" max="100" value="' + value + '" disabled aria-label="亮度预览"></div>';
            }
            if (kind === 'sleep-time' || kind === 'confirm-idle') {
                value = kind === 'sleep-time' ? finiteNumber(settings.idleSleepSeconds, 120) : finiteNumber(settings.controlConfirmIdleSeconds, 60);
                return '<div class="sp-system-preview sp-preset-preview"><button disabled aria-label="上一选项">−</button><output>' + (value ? value + ' 秒' : '永不') + '</output><button disabled aria-label="下一选项">＋</button></div>';
            }
            if (kind === 'touch-beep') return '<div class="sp-system-preview"><button role="switch" aria-checked="' + (settings.touchBeepEnabled !== false) + '" disabled>' + (settings.touchBeepEnabled !== false ? '开启' : '关闭') + '</button></div>';
            return '<div class="sp-system-preview"><button disabled>' + ({ sleep: '立即息屏', buzzer: '短响一次', restart: '重启设备' })[kind] + '</button></div>';
        },
        _spChoiceOptions(select, items, selected, placeholder) {
            if (!select) return;
            select.innerHTML = '';
            var empty = document.createElement('option');
            empty.value = '';
            empty.textContent = placeholder || '请选择';
            select.appendChild(empty);
            items.forEach(function(item) {
                var ref = choiceRef(item);
                if (!ref) return;
                var option = document.createElement('option');
                option.value = ref;
                option.textContent = choiceLabel(item) + (item.available === false ? '（不可用）' : '');
                option.disabled = item.available === false;
                select.appendChild(option);
            });
            select.value = selected || '';
        },

        _spMountInspector() {
            var host = document.getElementById("sp-inspector");
            if (host && !host.children.length) host.innerHTML = " <div id=\"sp-inspector-empty\" class=\"sp-inspector-empty\">选择画布组件设置属性</div> <form id=\"sp-inspector-form\" class=\"fb-hidden\" novalidate> <div class=\"sp-inspector-heading\"> <div><span id=\"sp-inspector-kind\" class=\"fb-tag fb-tag-primary\">组件</span><strong id=\"sp-inspector-id\"></strong></div> <button id=\"sp-card-delete\" type=\"button\" class=\"fb-btn fb-btn-sm fb-text-danger\">删除</button> </div> <label class=\"sp-field-stack\">名称 <input id=\"sp-card-title\" type=\"text\" maxlength=\"64\" autocomplete=\"off\"> <small id=\"sp-title-hint\">最多 2 行</small> </label> <label id=\"sp-source-field\" class=\"sp-field-stack\">数据来源 <select id=\"sp-card-source\"></select> </label> <div id=\"sp-bar-sources-field\" class=\"sp-field-stack fb-hidden\"> <label>对比来源 2<select id=\"sp-card-source-1\"></select></label> <label>对比来源 3<select id=\"sp-card-source-2\"></select></label> <label>对比来源 4<select id=\"sp-card-source-3\"></select></label> </div> <label id=\"sp-control-field\" class=\"sp-field-stack fb-hidden\">电机、开关或外设执行规则 <select id=\"sp-card-control\"></select> </label> <label id=\"sp-page-link-field\" class=\"sp-field-stack fb-hidden\">目标页面 <select id=\"sp-card-target-page\"></select> </label>  <label id=\"sp-display-field\" class=\"sp-field-stack\">显示形式 <select id=\"sp-card-display\"></select> </label> <div id=\"sp-motor-fields\" class=\"sp-inspector-grid fb-hidden\"> <small>首次/闲置确认：使用全局间隔</small> <label>初始速度<input id=\"sp-motor-speed\" type=\"number\" min=\"0\" max=\"65535\" value=\"50\"></label> <label>初始脉冲<input id=\"sp-motor-pulse\" type=\"number\" min=\"0\" max=\"65535\" value=\"1600\"></label> <label>速度步长<input id=\"sp-motor-speed-step\" type=\"number\" min=\"1\" max=\"65535\" value=\"1\"></label> <label>脉冲步长<input id=\"sp-motor-pulse-step\" type=\"number\" min=\"1\" max=\"65535\" value=\"100\"></label> </div> <div id=\"sp-numeric-fields\" class=\"sp-inspector-grid\"> <label>单位<input id=\"sp-card-unit\" type=\"text\" maxlength=\"16\"></label> <label>小数位<input id=\"sp-card-precision\" type=\"number\" min=\"0\" max=\"6\" value=\"1\"></label> <label>下限<input id=\"sp-card-min\" type=\"number\" step=\"any\" value=\"0\"></label> <label>上限<input id=\"sp-card-max\" type=\"number\" step=\"any\" value=\"100\"></label> <label id=\"sp-card-step-field\" class=\"fb-hidden\">步长<input id=\"sp-card-step\" type=\"number\" step=\"any\" value=\"1\"></label> <label id=\"sp-card-initial-field\" class=\"fb-hidden\">初始值<input id=\"sp-card-initial\" type=\"number\" step=\"any\" value=\"0\"></label> </div> <label id=\"sp-size-field\" class=\"sp-field-stack\">尺寸 <select id=\"sp-card-size\"><option value=\"small\">小</option><option value=\"wide\">宽</option><option value=\"large\">大</option></select> </label> <label id=\"sp-confirm-field\" class=\"sp-checkbox fb-hidden\"><input id=\"sp-card-confirm\" type=\"checkbox\">在物理屏执行前确认</label> <div class=\"sp-inspector-actions\"> <button id=\"sp-card-apply\" type=\"submit\" class=\"fb-btn fb-btn-primary\">应用设置</button> </div> </form> ";
            this._spRenderInspector();
        },
        _spRenderContentList() {
            var model = this._spEnsureModel(), host = document.getElementById('sp-content-list');
            if (!host) return;
            host.innerHTML = model.config.pages.map(function(page) {
                return '<div class="sp-content-page"><div class="sp-config-section-heading"><strong>' + escapeHtml(page.title) + '</strong><button class="fb-btn fb-btn-sm" data-sp-adjust-page="' + escapeHtml(page.id) + '">调整</button></div>' +
                    page.cards.map(function(card) {
                        var control = card.kind === 'motor' || card.kind === 'control';
                        if (!control && SOURCE_KINDS.indexOf(card.kind) < 0) return '<span class="sp-content-tag">' + escapeHtml(card.title) + '</span>';
                        var choices = model.choices.filter(function(choice) {
                            if (card.kind === 'motor') return choice.type === 'motor';
                            if (control) return isControlChoice(choice) && choice.type !== 'motor';
                            return card.kind === 'weather' ? isWeatherChoice(choice) : helpers.isNumericChoice(choice);
                        });
                        return '<label class="sp-content-source">' + escapeHtml(card.title) + '<select data-sp-content-source="' + escapeHtml(card.id) + '"><option value="">选择来源</option>' + choices.map(function(choice) {
                            var ref = choiceRef(choice);
                            return '<option value="' + escapeHtml(ref) + '"' + (ref === (control ? card.controlRef : card.sourceRef) ? ' selected' : '') + '>' + escapeHtml(choiceLabel(choice)) + '</option>';
                        }).join('') + '</select></label>';
                    }).join('') + '</div>';
            }).join('');
        },

        _spRenderWeather() {
            var weather = this._spEnsureModel().weather;
            if (!weather) return;
            document.getElementById('sp-weather-enabled').checked = weather.enabled === true;
            document.getElementById('sp-weather-location').value = weather.location || 'beijing';
            document.getElementById('sp-weather-interval').value = weather.intervalSeconds || 1800;
            var key = document.getElementById('sp-weather-key');
            key.value = ''; key.placeholder = weather.hasApiKey ? '已设置，留空保留' : '输入心知天气 API Key';
            var migration = weather.migration || {};
            document.getElementById('sp-weather-migration').classList.toggle('fb-hidden', migration.required !== true);
            document.getElementById('sp-weather-legacy').innerHTML = '<option value="">请选择</option>' + (migration.candidates || []).filter(function(item) { return item.compatible; }).map(function(item) {
                return '<option value="' + escapeHtml(item.id) + '">' + escapeHtml(item.label || item.location || item.id) + '</option>';
            }).join('') + '<option value="new">重新配置心知天气</option>';
            this._spWeatherStatus(weather.status);
        },

        _spWeatherStatus(status) {
            status = status || {};
            var preview = status.preview || status.weather || {};
            document.getElementById('sp-weather-status').textContent = status.message || status.error ||
                (status.pending ? '正在更新天气' : preview.temperature !== undefined ?
                    [preview.city, preview.description, preview.temperature + '℃', preview.providerUpdatedAt || ''].filter(Boolean).join(' · ') : '保存后可更新天气');
        },

        async _spRefreshWeather() {
            var model = this._spEnsureModel();
            if (!model.weather || model.weatherDirty) { this._spSetMessage('请先保存天气配置', 'error'); return; }
            var button = document.getElementById('sp-weather-refresh'); button.disabled = true;
            try {
                var response = await apiPost('/api/screen-panel/weather/refresh', { id: model.weather.id });
                if (!response || !response.success) throw new Error(response && (response.error || response.message) || '天气更新失败');
                this._spWeatherStatus(response.data);
                if (response.data.accepted) {
                    for (var i = 0; i < 15 && this.currentPage === 'screen-panel'; i++) {
                        await new Promise(function(resolve) { setTimeout(resolve, 1000); });
                        if (this.currentPage !== 'screen-panel') break;
                        response = await apiGet('/api/screen-panel/weather/refresh', { id: model.weather.id });
                        this._spWeatherStatus(response.data);
                        if (!(response.data && response.data.pending)) break;
                    }
                }
            } catch (error) { this._spSetMessage(error.data && (error.data.error || error.data.message) || error.message || '天气更新失败', 'error'); }
            finally { button.disabled = false; }
        },

        async _spSaveAll() {
            if (!this.guardDeveloperModeAction()) return false;
            var model = this._spEnsureModel();
            if (model.saving) return false;
            var validation = this._spValidateConfig(model.config);
            if (validation.errors.length) { this._spSetMessage(validation.errors.join('；'), 'error'); return false; }
            var saved = [];
            model.saving = true;
            var form = document.getElementById('sp-config'); form.inert = true;
            try {
                if (model.weatherDirty) {
                    if (!model.weather) throw new Error('天气配置尚未加载');
                    var response = await apiPostJson('/api/screen-panel/weather', {
                        expectedRevision: model.weather.revision,
                        enabled: document.getElementById('sp-weather-enabled').checked,
                        location: document.getElementById('sp-weather-location').value.trim(),
                        intervalSeconds: Number(document.getElementById('sp-weather-interval').value),
                        apiKey: document.getElementById('sp-weather-key').value,
                        legacySourceId: document.getElementById('sp-weather-legacy').value
                    });
                    if (!response || !response.success) throw new Error(response && (response.error || response.message) || '天气保存失败');
                    model.weather = response.data; model.weatherDirty = false; saved.push('天气'); this._spRenderWeather();
                    var choices = await this._spLoadAllChoices(apiGetFresh);
                    model.choices = choices.items || model.choices;
                }
                var sources = Array.from(new Set(model.config.pages.flatMap(function(page) { return page.cards.filter(function(card) { return card.kind === 'line'; }).map(function(card) { return card.sourceRef; }).filter(Boolean); })));
                if (sources.length) {
                    if (!model.historyConfig) throw new Error('设备未提供最近记录设置');
                    var old = model.historyConfig;
                    var allowlist = Array.isArray(old.sourceAllowlist) ? old.sourceAllowlist : [];
                    var next = old.enabled && !allowlist.length ? [] : Array.from(new Set(allowlist.concat(sources)));
                    if (next.length > finiteNumber(old.maxSources, 16)) throw new Error('最近记录来源超过设备上限');
                    if (!old.enabled || JSON.stringify(next) !== JSON.stringify(allowlist)) {
                        var history = await apiPostJson('/api/periph-exec/history/config', { enabled: true, sourceAllowlist: next });
                        if (!history || !history.success) throw new Error('最近记录设置保存失败');
                        model.historyConfig = history.data.config; saved.push('最近记录');
                    }
                }
                if ((model.dirty || !model.persisted) && !await this._spSave()) {
                    var detail = document.getElementById('sp-page-message');
                    throw new Error(detail && detail.textContent || '屏幕布局未保存，请检查提示后重试');
                }
                this._spRenderAll(); this._spSetMessage(model.application && model.application.pendingRestart ? '已保存，重启后应用到物理屏' : '配置已保存并提交应用', 'success'); return true;
            } catch (error) {
                this._spSetMessage((saved.length ? saved.join('、') + '已保存；' : '') + (error.data && (error.data.error || error.data.message) || error.message || '保存失败'), 'error'); return false;
            } finally { model.saving = false; form.inert = false; this._spRenderDirtyState(); }
        },

    };
})();
