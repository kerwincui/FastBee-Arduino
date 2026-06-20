/**
 * FastBee UI 工具函数
 */
(function() {
    'use strict';

    // HTML 转义（统一替代各模块的 _esc() 函数）
    window.escapeHtml = function(text) {
        if (text == null) return '';
        var map = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };
        return String(text).replace(/[&<>"']/g, function(m) { return map[m]; });
    };

    // 安全设置文本内容
    window.safeText = function(el, text) {
        if (el) el.textContent = text != null ? String(text) : '';
    };

    // 安全设置 HTML（已转义）
    window.safeHtml = function(el, html) {
        if (el) el.innerHTML = html;
    };

    /**
     * 字节数格式化（自动选择 B/KB/MB/GB 单位）
     * @param {number} bytes - 字节数
     * @returns {string} 格式化后的字符串，如 "1.25 MB"
     */
    window.formatBytes = function(bytes) {
        if (!bytes || bytes === 0) return '0 B';
        var k = 1024;
        var sizes = ['B', 'KB', 'MB', 'GB'];
        var i = Math.floor(Math.log(bytes) / Math.log(k));
        if (i >= sizes.length) i = sizes.length - 1;
        return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];
    };

    /**
     * 运行时间格式化（秒数转为 Xh Xm Xs）
     * @param {number} seconds - 秒数
     * @returns {string} 格式化后的字符串，如 "2h 15m 30s"
     */
    window.formatUptime = function(seconds) {
        if (seconds == null || seconds < 0) return '--';
        seconds = Math.floor(seconds);
        var h = Math.floor(seconds / 3600);
        var m = Math.floor((seconds % 3600) / 60);
        var s = seconds % 60;
        if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
        if (m > 0) return m + 'm ' + s + 's';
        return s + 's';
    };

    /**
     * 防抖函数
     * @param {Function} fn - 需要防抖的函数
     * @param {number} delay - 延迟毫秒数（默认 300）
     * @returns {Function} 防抖后的函数
     */
    window.debounce = function(fn, delay) {
        var timer = null;
        delay = delay || 300;
        return function() {
            var ctx = this, args = arguments;
            if (timer) clearTimeout(timer);
            timer = setTimeout(function() {
                fn.apply(ctx, args);
            }, delay);
        };
    };

    /**
     * 数值限制在 [min, max] 范围内
     * @param {number} value - 输入值
     * @param {number} min - 最小值
     * @param {number} max - 最大值
     * @returns {number}
     */
    window.clamp = function(value, min, max) {
        return Math.max(min, Math.min(max, value));
    };

    /**
     * 深拷贝（基于 JSON 序列化，适用于纯数据对象）
     * @param {*} obj - 需要拷贝的对象
     * @returns {*} 深拷贝后的新对象
     */
    window.deepClone = function(obj) {
        if (obj == null || typeof obj !== 'object') return obj;
        return JSON.parse(JSON.stringify(obj));
    };

    /**
     * 配置导入字段过滤：以 reference（当前设备配置）为 schema，
     * 过滤 imported 中的多余/不匹配字段，只保留 reference 中已有的字段。
     * - 嵌套对象：递归过滤
     * - 数组字段：保留导入的数组（用户数据如 Modbus 任务列表）
     * - 导入中缺失的字段：保留 reference 中的当前值
     * - 多余字段：静默丢弃
     *
     * @param {object} imported - 导入的配置对象
     * @param {object} reference - 当前设备上的配置对象（作为 schema）
     * @returns {object} 过滤后的配置对象
     */
    window.filterConfigFields = function(imported, reference) {
        // 非对象无法过滤，直接返回导入值
        if (typeof imported !== 'object' || imported === null ||
            typeof reference !== 'object' || reference === null) {
            return imported;
        }
        // reference 是数组 → 保留导入的数组
        if (Array.isArray(reference)) {
            return Array.isArray(imported) ? imported : reference;
        }
        // imported 不是普通对象 → 返回 reference
        if (Array.isArray(imported)) {
            return reference;
        }
        var result = {};
        for (var key in reference) {
            if (!reference.hasOwnProperty(key)) continue;
            // 导入中没有该字段 → 保留当前值
            if (!(key in imported)) {
                result[key] = reference[key];
                continue;
            }
            var refVal = reference[key];
            var impVal = imported[key];
            // 两侧都是普通对象 → 递归过滤
            if (refVal !== null && typeof refVal === 'object' && !Array.isArray(refVal) &&
                impVal !== null && typeof impVal === 'object' && !Array.isArray(impVal)) {
                result[key] = window.filterConfigFields(impVal, refVal);
            } else if (Array.isArray(refVal)) {
                // reference 侧是数组 → 保留导入的数组（允许用户自定义列表）
                result[key] = Array.isArray(impVal) ? impVal : refVal;
            } else {
                // 基本类型字段 → 使用导入的值
                result[key] = impVal;
            }
        }
        return result;
    };
})();
