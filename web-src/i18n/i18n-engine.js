// i18n 核心引擎 - 不包含翻译数据
// 翻译数据通过 i18n-zh-CN.js 和 i18n-en.js 动态加载

var i18n = typeof i18n !== 'undefined' ? i18n : {
    currentLang: localStorage.getItem('language') || 'zh-CN',
    
    // 已加载的语言包列表
    loadedLanguages: ['zh-CN'],
    
    // 正在加载的语言包
    loadingLanguages: [],
    
    // 翻译数据容器（由语言包文件填充）
    translations: {
        'zh-CN': {},
        'en': {}
    },
    
    // 翻译函数
    t(key) {
        const translated = this.translations[this.currentLang] && this.translations[this.currentLang][key];
        if (!translated && typeof console !== 'undefined' && console.warn) {
            // 仅在开发/调试模式下输出警告，避免生产环境控制台噪声
            if (window.DEBUG || location.hostname === 'localhost' || location.hostname === '127.0.0.1' || location.hostname === '192.168.4.1') {
                console.warn('[i18n] Missing key:', key, 'in', this.currentLang || 'unknown');
            }
        }
        return translated || key;
    },
    
    // 添加翻译数据（供语言包动态加载后调用）
    addTranslations(lang, data) {
        if (!this.translations[lang]) {
            this.translations[lang] = {};
        }
        // 合并翻译数据
        Object.assign(this.translations[lang], data);
        // 标记为已加载
        if (!this.loadedLanguages.includes(lang)) {
            this.loadedLanguages.push(lang);
        }
        // 从加载中列表移除
        const loadingIndex = this.loadingLanguages.indexOf(lang);
        if (loadingIndex > -1) {
            this.loadingLanguages.splice(loadingIndex, 1);
        }
    },
    
    // 合并翻译数据（兼容方法）
    mergeTranslations(lang, data) {
        this.addTranslations(lang, data);
    },
    
    // 检查语言包是否已加载
    isLanguageLoaded(lang) {
        return this.loadedLanguages.includes(lang);
    },
    
    // 检查语言包是否正在加载
    isLanguageLoading(lang) {
        return this.loadingLanguages.indexOf(lang) > -1;
    },
    
    // 动态加载语言包
    loadLanguagePack(lang, callback) {
        // 如果已加载，直接回调
        if (this.isLanguageLoaded(lang)) {
            if (callback) callback(true);
            return;
        }
        
        // 如果正在加载，等待加载完成
        if (this.isLanguageLoading(lang)) {
            // 轮询等待加载完成（最多等待5秒）
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
        
        // 标记为正在加载
        this.loadingLanguages.push(lang);
        
        // 动态创建script标签加载语言包
        const script = document.createElement('script');
        script.src = './js/modules/i18n-' + lang + '.js';
        script.onload = () => {
            // 语言包加载成功后会自动调用 addTranslations
            if (callback) callback(true);
        };
        script.onerror = () => {
            // 加载失败，从加载中列表移除
            const loadingIndex = this.loadingLanguages.indexOf(lang);
            if (loadingIndex > -1) {
                this.loadingLanguages.splice(loadingIndex, 1);
            }
            console.error('Failed to load language pack: ' + lang);
            if (callback) callback(false);
        };
        document.head.appendChild(script);
    },
    
    // 切换语言
    setLanguage(lang) {
        const doSwitch = () => {
            if (this.translations[lang]) {
                this.currentLang = lang;
                localStorage.setItem('language', lang);
                this.updatePageText();
                
                // 显示语言切换成功的消息
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
        
        // 如果语言包未加载，先动态加载
        if (!this.isLanguageLoaded(lang) && lang !== 'zh-CN') {
            this.loadLanguagePack(lang, (success) => {
                if (success) {
                    doSwitch();
                } else {
                    // 加载失败，回退到中文
                    if (typeof Notification !== 'undefined') {
                        Notification.error(
                            lang === 'zh-CN' ? '语言包加载失败，已回退到中文' : 'Language pack load failed, fallback to Chinese',
                            lang === 'zh-CN' ? '错误' : 'Error'
                        );
                    }
                    this.setLanguage('zh-CN');
                }
            });
            return false; // 异步加载，返回false表示还未切换完成
        }
        
        return doSwitch();
    },
    
    // 更新页面文本
    updatePageText() {
        // 1. 更新所有有 data-i18n 属性的元素（统一入口，替代旧的 [id] 全量扫描）
        document.querySelectorAll('[data-i18n]').forEach(element => {
            const key = element.getAttribute('data-i18n');
            const translation = this.t(key);
            if (translation && translation !== key) {
                if (element.tagName === 'INPUT' || element.tagName === 'TEXTAREA') {
                    if (element.type === 'button' || element.type === 'submit') {
                        // INPUT button/submit: 设置 value 属性
                        element.value = translation;
                    } else {
                        // 其他 INPUT/TEXTAREA: 设置 placeholder
                        element.placeholder = translation;
                    }
                } else if (element.tagName === 'SELECT') {
                    // 跳过 SELECT 元素，textContent 会清除其 option 子节点
                    return;
                } else if (element.tagName === 'OPTION') {
                    element.textContent = translation;
                    // 更新 select 的显示文本
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

        // 2. 更新有 data-i18n-placeholder 属性的输入框占位符
        document.querySelectorAll('[data-i18n-placeholder]').forEach(element => {
            const key = element.getAttribute('data-i18n-placeholder');
            const translation = this.t(key);
            if (translation && translation !== key) {
                element.placeholder = translation;
            }
        });

        // 3. 更新有 data-i18n-title 属性的元素 title 属性
        document.querySelectorAll('[data-i18n-title]').forEach(element => {
            const key = element.getAttribute('data-i18n-title');
            const translation = this.t(key);
            if (translation && translation !== key) {
                element.title = translation;
            }
        });

        // 4. 更新 html lang 属性
        document.documentElement.lang = this.currentLang;
    }
};
