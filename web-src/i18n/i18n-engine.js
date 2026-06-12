// i18n 引擎 - 默认中文，Full版本支持中英文切换
var i18n = typeof i18n !== 'undefined' ? i18n : (function() {
    var _translations = {};
    var _currentLang = 'zh-CN';
    var _zhLoaded = false;
    var _enLoaded = false;
    var _loading = false;

    return {
        currentLang: _currentLang,
        _zhLoaded: false,
        _enLoaded: false,

        /**
         * 翻译键值
         * 中文硬编码在HTML中，仅英文需要翻译替换
         */
        t: function(key) {
            if (!key) return '';
            var langData = _translations[_currentLang];
            if (langData && langData[key] !== undefined) {
                return langData[key];
            }
            // 回退到中文
            var zhData = _translations['zh-CN'];
            if (zhData && zhData[key] !== undefined) {
                return zhData[key];
            }
            return key;
        },

        /**
         * 添加翻译数据
         */
        addTranslations: function(lang, data) {
            if (!lang || !data) return;
            _translations[lang] = data;
            if (lang === 'zh-CN') this._zhLoaded = true;
            if (lang === 'en') this._enLoaded = true;
        },

        /**
         * 合并翻译数据（增量）
         */
        mergeTranslations: function(lang, data) {
            if (!lang || !data) return;
            if (!_translations[lang]) _translations[lang] = {};
            var target = _translations[lang];
            var keys = Object.keys(data);
            for (var i = 0; i < keys.length; i++) {
                target[keys[i]] = data[keys[i]];
            }
        },

        isLanguageLoaded: function(lang) {
            if (lang === 'zh-CN') return this._zhLoaded;
            if (lang === 'en') return this._enLoaded;
            return !!_translations[lang];
        },

        isLanguageLoading: function() {
            return _loading;
        },

        /**
         * 按需加载语言包脚本
         */
        loadLanguagePack: function(lang, cb) {
            var self = this;
            if (self.isLanguageLoaded(lang)) {
                if (cb) cb(true);
                return;
            }
            _loading = true;
            var fileMap = { 'zh-CN': 'i18n-zh-CN', 'en': 'i18n-en' };
            var fileName = fileMap[lang];
            if (!fileName) {
                _loading = false;
                if (cb) cb(false);
                return;
            }
            var script = document.createElement('script');
            var basePath = './js/modules/' + fileName + '.js';
            script.src = (typeof window.__fastbeeResolveAssetUrl === 'function')
                ? window.__fastbeeResolveAssetUrl(basePath)
                : basePath;
            script.onload = function() {
                _loading = false;
                if (cb) cb(true);
            };
            script.onerror = function() {
                _loading = false;
                if (cb) cb(false);
            };
            document.head.appendChild(script);
        },

        /**
         * 切换语言
         */
        setLanguage: function(lang, cb) {
            var self = this;
            _currentLang = lang || 'zh-CN';
            self.currentLang = _currentLang;
            localStorage.setItem('language', _currentLang);

            if (self.isLanguageLoaded(_currentLang)) {
                self.updatePageText();
                if (cb) cb(true);
                return true;
            }

            // 按需加载语言包
            self.loadLanguagePack(_currentLang, function(ok) {
                if (ok) self.updatePageText();
                if (cb) cb(ok);
            });
            return true;
        },

        /**
         * 更新页面所有 data-i18n 元素的文本
         */
        updatePageText: function(root) {
            var scope = root || document;
            var elements = scope.querySelectorAll('[data-i18n]');
            for (var i = 0; i < elements.length; i++) {
                var el = elements[i];
                var key = el.getAttribute('data-i18n');
                if (!key) continue;
                var translation = this.t(key);
                // 仅当翻译不等于 key 时才替换（避免丢失硬编码中文）
                if (translation !== key) {
                    el.textContent = translation;
                }
            }
            // placeholder 翻译
            var placeholders = scope.querySelectorAll('[data-i18n-placeholder]');
            for (var j = 0; j < placeholders.length; j++) {
                var pel = placeholders[j];
                var pkey = pel.getAttribute('data-i18n-placeholder');
                if (!pkey) continue;
                var ptrans = this.t(pkey);
                if (ptrans !== pkey) {
                    pel.placeholder = ptrans;
                }
            }
        }
    };
})();
