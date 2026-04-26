/**
 * FastBee UI 组件库
 * 提供常用 UI 元素的 DOM 创建方法，替代 innerHTML 拼接
 */
(function() {
    'use strict';

    var FB = window.FB || {};

    FB.ui = {
        // 创建按钮
        createButton: function(text, options) {
            options = options || {};
            var btn = document.createElement('button');
            btn.type = options.type || 'button';
            btn.className = options.className || 'fb-btn fb-btn-primary';
            btn.textContent = text;
            if (options.onclick) btn.addEventListener('click', options.onclick);
            if (options.disabled) btn.disabled = true;
            if (options.dataset) {
                var keys = Object.keys(options.dataset);
                for (var i = 0; i < keys.length; i++) {
                    btn.dataset[keys[i]] = options.dataset[keys[i]];
                }
            }
            return btn;
        },

        // 创建表格行
        createTableRow: function(cells, options) {
            options = options || {};
            var tr = document.createElement('tr');
            if (options.className) tr.className = options.className;
            for (var i = 0; i < cells.length; i++) {
                var cell = cells[i];
                var td = document.createElement('td');
                if (typeof cell === 'string') {
                    td.textContent = cell;  // 安全：使用 textContent
                } else if (cell instanceof Node) {
                    td.appendChild(cell);
                } else if (cell && cell.html) {
                    td.innerHTML = cell.html;  // 明确标记为 HTML
                } else if (cell && cell.className) {
                    td.className = cell.className;
                    if (cell.text) td.textContent = cell.text;
                }
                if (options.cellClassNames && options.cellClassNames[i]) {
                    td.className = options.cellClassNames[i];
                }
                tr.appendChild(td);
            }
            return tr;
        },

        // 创建通知
        createNotification: function(type, message, duration) {
            duration = duration || 3000;
            var div = document.createElement('div');
            div.className = 'notification notification-' + type;
            div.textContent = message;
            document.body.appendChild(div);
            setTimeout(function() { div.remove(); }, duration);
            return div;
        },

        // 创建 DocumentFragment（批量 DOM 操作）
        createFragment: function() {
            return document.createDocumentFragment();
        },

        // 清空容器并批量添加子元素
        replaceChildren: function(container, children) {
            var fragment = document.createDocumentFragment();
            for (var i = 0; i < children.length; i++) {
                fragment.appendChild(children[i]);
            }
            container.innerHTML = '';
            container.appendChild(fragment);
        }
    };

    window.FB = FB;
})();
