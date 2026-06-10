// i18n core engine. Translation data is loaded from zh-CN/en language packs.
var __fastbeeCriticalZh = typeof __fastbeeCriticalZh !== 'undefined' ? __fastbeeCriticalZh : {
    'login-title': '设备配置管理',
    'username-label': '用户名',
    'username-placeholder': '请输入用户名',
    'password-label': '密码',
    'password-placeholder': '请输入密码',
    'remember-label': '记住密码',
    'login-button': '登录系统',
    'login-logging-in': '登录中...',
    'login-logging-in-html': '… 登录中...',
    'login-success-msg': '登录成功',
    'login-welcome-title': '欢迎',
    'login-fail-title': '登录失败',
    'login-fail-msg': '用户名或密码错误',
    'login-empty-warning': '请输入用户名和密码'
};

var i18n = typeof i18n !== 'undefined' ? i18n : {
    currentLang: localStorage.getItem('language') || 'zh-CN',
    loadedLanguages: ['zh-CN'],
    loadingLanguages: [],
    translations: {
        'zh-CN': Object.assign({}, __fastbeeCriticalZh),
        en: {}
    },

    t(key) {
        var translated = this.translations[this.currentLang] && this.translations[this.currentLang][key];
        if (!translated && this.currentLang !== 'zh-CN') {
            translated = this.translations['zh-CN'] && this.translations['zh-CN'][key];
        }
        if (!translated && typeof console !== 'undefined' && console.warn) {
            if (window.DEBUG || location.hostname === 'localhost' || location.hostname === '127.0.0.1' || location.hostname === '192.168.4.1') {
                console.warn('[i18n] Missing key:', key, 'in', this.currentLang || 'unknown');
            }
        }
        return translated || key;
    },

    addTranslations(lang, data) {
        if (!this.translations[lang]) {
            this.translations[lang] = {};
        }
        Object.assign(this.translations[lang], data || {});
        if (!this.loadedLanguages.includes(lang)) {
            this.loadedLanguages.push(lang);
        }
        const loadingIndex = this.loadingLanguages.indexOf(lang);
        if (loadingIndex > -1) {
            this.loadingLanguages.splice(loadingIndex, 1);
        }
    },

    mergeTranslations(lang, data) {
        this.addTranslations(lang, data);
    },

    isLanguageLoaded(lang) {
        return this.loadedLanguages.includes(lang);
    },

    isLanguageLoading(lang) {
        return this.loadingLanguages.indexOf(lang) > -1;
    },

    loadLanguagePack(lang, callback) {
        if (this.isLanguageLoaded(lang)) {
            if (callback) callback(true);
            return;
        }

        if (this.isLanguageLoading(lang)) {
            let waitCount = 0;
            const waitInterval = setInterval(() => {
                waitCount++;
                if (this.isLanguageLoaded(lang)) {
                    clearInterval(waitInterval);
                    if (callback) callback(true);
                } else if (waitCount > 50) {
                    clearInterval(waitInterval);
                    if (callback) callback(false);
                }
            }, 100);
            return;
        }

        this.loadingLanguages.push(lang);

        const script = document.createElement('script');
        const packPath = './js/modules/i18n-' + lang + '.js';
        script.src = (typeof window.__fastbeeResolveAssetUrl === 'function')
            ? window.__fastbeeResolveAssetUrl(packPath)
            : packPath;
        script.onload = () => {
            if (callback) callback(true);
        };
        script.onerror = () => {
            const loadingIndex = this.loadingLanguages.indexOf(lang);
            if (loadingIndex > -1) {
                this.loadingLanguages.splice(loadingIndex, 1);
            }
            console.error('Failed to load language pack: ' + lang);
            if (callback) callback(false);
        };
        document.head.appendChild(script);
    },

    setLanguage(lang) {
        const doSwitch = () => {
            if (this.translations[lang]) {
                this.currentLang = lang;
                localStorage.setItem('language', lang);
                this.updatePageText();

                if (typeof Notification !== 'undefined') {
                    Notification.success(
                        lang === 'zh-CN' ? '语言已切换为中文' : 'Language switched to English',
                        lang === 'zh-CN' ? '成功' : 'Success'
                    );
                }
                return true;
            }
            return false;
        };

        if (!this.isLanguageLoaded(lang) && lang !== 'zh-CN') {
            this.loadLanguagePack(lang, (success) => {
                if (success) {
                    doSwitch();
                    return;
                }
                if (typeof Notification !== 'undefined') {
                    Notification.error(
                        lang === 'zh-CN' ? '语言包加载失败，已回退到中文' : 'Language pack load failed, fallback to Chinese',
                        lang === 'zh-CN' ? '错误' : 'Error'
                    );
                }
                this.setLanguage('zh-CN');
            });
            return false;
        }

        return doSwitch();
    },

    updatePageText(root) {
        const scope = root && root.querySelectorAll ? root : document;
        const each = (selector, handler) => {
            if (scope.matches && scope.matches(selector)) handler(scope);
            scope.querySelectorAll(selector).forEach(handler);
        };

        each('[data-i18n]', element => {
            const key = element.getAttribute('data-i18n');
            const translation = this.t(key);
            if (translation && translation !== key) {
                if (element.tagName === 'INPUT' || element.tagName === 'TEXTAREA') {
                    if (element.type === 'button' || element.type === 'submit') {
                        element.value = translation;
                    } else {
                        element.placeholder = translation;
                    }
                } else if (element.tagName === 'SELECT') {
                    return;
                } else if (element.tagName === 'OPTION') {
                    element.textContent = translation;
                    const select = element.parentElement;
                    if (select && select.tagName === 'SELECT') {
                        const idx = select.selectedIndex;
                        select.selectedIndex = idx;
                    }
                } else {
                    element.textContent = translation;
                }
            }
        });

        each('[data-i18n-placeholder]', element => {
            const key = element.getAttribute('data-i18n-placeholder');
            const translation = this.t(key);
            if (translation && translation !== key) {
                element.placeholder = translation;
            }
        });

        each('[data-i18n-title]', element => {
            const key = element.getAttribute('data-i18n-title');
            const translation = this.t(key);
            if (translation && translation !== key) {
                element.title = translation;
            }
        });

        document.documentElement.lang = this.currentLang;
    }
};

if (typeof i18n !== 'undefined' && i18n.addTranslations) {
    i18n.addTranslations('zh-CN', __fastbeeCriticalZh);
}
