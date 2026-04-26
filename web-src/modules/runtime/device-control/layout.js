/**
 * device-control/layout.js — 自由拖拽/缩放布局管理
 */
(function() {
    'use strict';

    Object.assign(AppState, {

        // ============ 自由拖拽布局相关属性和方法 ============
        _DC_LAYOUT_KEY: 'dc-card-layout',
        _dcDragEl: null,
        _dcDragOffset: null,
        _dcResizeEl: null,
        _dcResizeStart: null,

        _dcInitFreeLayout: function() {
            var flow = document.querySelector('.dc-control-flow');
            if (!flow || flow._dcLayoutBound) return;
            var self = this;

            flow.addEventListener('mousedown', function(e) {
                var card = e.target.closest('[data-dc-sort-key]');
                if (!card) return;

                // 检测 resize handle
                var resizeHandle = e.target.closest('.dc-resize-handle');
                if (resizeHandle) {
                    e.preventDefault();
                    e.stopPropagation();
                    self._dcResizeEl = card;
                    self._dcResizeStart = {
                        x: e.clientX,
                        y: e.clientY,
                        w: card.offsetWidth,
                        h: card.offsetHeight
                    };
                    card.classList.add('dc-resizing');
                    return;
                }

                // 排除控件点击
                if (e.target.closest('button,input,select,textarea,.dc-pwm-slider')) return;
                e.preventDefault();
                self._dcDragEl = card;
                var rect = card.getBoundingClientRect();
                self._dcDragOffset = { x: e.clientX - rect.left, y: e.clientY - rect.top };
                card.classList.add('dc-dragging');
                card.style.zIndex = '100';
            });

            document.addEventListener('mousemove', function(e) {
                // 处理 resize
                if (self._dcResizeEl) {
                    var dx = e.clientX - self._dcResizeStart.x;
                    var dy = e.clientY - self._dcResizeStart.y;
                    var newW = Math.max(200, Math.min(self._dcResizeStart.w + dx, flow.clientWidth));
                    var newH = Math.max(100, self._dcResizeStart.h + dy);
                    self._dcResizeEl.style.width = newW + 'px';
                    self._dcResizeEl.style.height = newH + 'px';
                    return;
                }

                // 处理 drag
                if (!self._dcDragEl) return;
                var flowRect = flow.getBoundingClientRect();
                if (!flowRect) return;
                var x = e.clientX - flowRect.left - self._dcDragOffset.x;
                var y = e.clientY - flowRect.top - self._dcDragOffset.y;
                x = Math.max(0, Math.min(x, flow.clientWidth - self._dcDragEl.offsetWidth));
                y = Math.max(0, y);
                self._dcDragEl.style.left = x + 'px';
                self._dcDragEl.style.top = y + 'px';
            });

            document.addEventListener('mouseup', function() {
                // 处理 resize 完成
                if (self._dcResizeEl) {
                    self._dcResizeEl.classList.remove('dc-resizing');
                    self._dcSaveLayout();
                    self._dcResizeEl = null;
                    self._dcResizeStart = null;
                    return;
                }

                // 处理 drag 完成
                if (!self._dcDragEl) return;
                self._dcDragEl.classList.remove('dc-dragging');
                self._dcDragEl.style.zIndex = '';
                self._dcDragEl = null;
                self._dcSaveLayout();
            });

            flow._dcLayoutBound = true;
        },

        _dcApplyLayout: function() {
            var flow = document.querySelector('.dc-control-flow');
            if (!flow) return;
            var cards = flow.querySelectorAll('[data-dc-sort-key]');
            if (cards.length === 0) return;

            var saved = null;
            try { saved = JSON.parse(localStorage.getItem(this._DC_LAYOUT_KEY)); } catch(e) {}

            var maxBottom = 0;
            if (saved && typeof saved === 'object') {
                for (var i = 0; i < cards.length; i++) {
                    var key = cards[i].getAttribute('data-dc-sort-key');
                    var pos = saved[key];
                    if (pos) {
                        cards[i].style.left = pos.x + 'px';
                        cards[i].style.top = pos.y + 'px';
                        if (pos.w) {
                            cards[i].style.width = pos.w + 'px';
                        }
                        if (pos.h) {
                            cards[i].style.height = pos.h + 'px';
                        }
                    } else {
                        cards[i].style.left = '0px';
                        cards[i].style.top = maxBottom + 'px';
                    }
                    var bottom = parseInt(cards[i].style.top) + cards[i].offsetHeight;
                    if (bottom > maxBottom) maxBottom = bottom;
                }
            } else {
                this._dcAutoGridLayout(flow, cards);
                for (var i = 0; i < cards.length; i++) {
                    var bottom = parseInt(cards[i].style.top || 0) + cards[i].offsetHeight;
                    if (bottom > maxBottom) maxBottom = bottom;
                }
            }
            flow.style.minHeight = (maxBottom + 20) + 'px';
        },

        _dcAutoGridLayout: function(flow, cards) {
            var gap = 15;
            var colWidth = 340 + gap;
            var cols = Math.max(1, Math.floor(flow.clientWidth / colWidth));
            var colTops = [];
            for (var c = 0; c < cols; c++) colTops.push(0);

            for (var i = 0; i < cards.length; i++) {
                var key = cards[i].getAttribute('data-dc-sort-key');
                var isWide = (key === 'health' || key === 'monitor-data');
                var span = (isWide && cols >= 2) ? 2 : 1;

                var bestCol = 0;
                var bestTop = Infinity;
                for (var c = 0; c <= cols - span; c++) {
                    var maxTop = 0;
                    for (var s = 0; s < span; s++) {
                        if (colTops[c + s] > maxTop) maxTop = colTops[c + s];
                    }
                    if (maxTop < bestTop) { bestTop = maxTop; bestCol = c; }
                }

                cards[i].style.left = (bestCol * colWidth) + 'px';
                cards[i].style.top = bestTop + 'px';

                var cardHeight = cards[i].offsetHeight + gap;
                for (var s = 0; s < span; s++) {
                    colTops[bestCol + s] = bestTop + cardHeight;
                }
            }
        },

        _dcSaveLayout: function() {
            var flow = document.querySelector('.dc-control-flow');
            if (!flow) return;
            var cards = flow.querySelectorAll('[data-dc-sort-key]');
            var layout = {};
            var maxBottom = 0;
            for (var i = 0; i < cards.length; i++) {
                var key = cards[i].getAttribute('data-dc-sort-key');
                var layoutData = { x: parseInt(cards[i].style.left) || 0, y: parseInt(cards[i].style.top) || 0 };
                if (cards[i].style.width) {
                    layoutData.w = parseInt(cards[i].style.width, 10);
                }
                if (cards[i].style.height) {
                    layoutData.h = parseInt(cards[i].style.height, 10);
                }
                layout[key] = layoutData;
                var bottom = layoutData.y + cards[i].offsetHeight;
                if (bottom > maxBottom) maxBottom = bottom;
            }
            flow.style.minHeight = (maxBottom + 20) + 'px';
            try { localStorage.setItem(this._DC_LAYOUT_KEY, JSON.stringify(layout)); } catch(e) {}
        },

        _dcResetLayout: function() {
            try { localStorage.removeItem(this._DC_LAYOUT_KEY); } catch(e) {}
            var flow = document.querySelector('.dc-control-flow');
            if (!flow) return;
            var cards = flow.querySelectorAll('[data-dc-sort-key]');
            for (var i = 0; i < cards.length; i++) {
                cards[i].style.width = '';
                cards[i].style.height = '';
            }
            this._dcAutoGridLayout(flow, cards);
            this._dcSaveLayout();
        }
    });
})();
