/**
 * 外设执行编辑器 - 灯效与显示扩展
 * 仅在选择 NeoPixel/OLED 动作时按需加载。
 */
(function() {
    var t = typeof window.t === 'function' ? window.t : function(key, fallback) { return fallback || key; };
    AppState.registerModule('periph-exec-effects', {
        _buildPeriphExecEffectsDisplayPanel(actionType, actionValue) {
            if (actionType === 27) {
                return '<textarea class="pe-action-value-oled pe-script-textarea" rows="6" maxlength="512" placeholder="多行文本，支持传感器模板">' + escapeHtml(actionValue || '') + '</textarea>' +
                    '<small class="pe-oled-counter">' + this._formatOledCounterStats(actionValue || '') + '</small>' +
                    '<small class="pe-help-text">最多 6 行；支持 ${periphId.field} 和 $value 模板</small>';
            }
            if (actionType === 11 || actionType === 10) {
                return '<select class="pe-neopixel-preset"><option value="">-- 选择灯效 --</option>' +
                    '<option value="red">red-红色</option><option value="green">green-绿色</option><option value="blue">blue-蓝色</option>' +
                    '<option value="yellow">yellow-黄色</option><option value="orange">orange-橙色</option><option value="purple">purple-紫色</option>' +
                    '<option value="cyan">cyan-青色</option><option value="white">white-白色</option><option value="off">off-关闭</option>' +
                    '<option value="rainbow">rainbow-彩虹循环</option><option value="chase">chase-流水灯</option><option value="theater_chase">theater_chase-剧院追逐</option>' +
                    '<option value="strobe">strobe-频闪</option><option value="twinkle">twinkle-闪烁</option><option value="fade">fade-渐变</option>' +
                    '<option value="breathing">breathing-呼吸</option><option value="color_wipe">color_wipe-色彩擦除</option><option value="fire">fire-火焰</option>' +
                    '<option value="platform_color">$value-平台下发</option><option value="custom">custom-自定义</option></select>' +
                    '<small class="pe-neopixel-help">选择预设灯效，或在“自定义”中输入 JSON 参数</small>';
            }
            return '';
        },

        _formatOledCounterStats(text) {
            var len = (text || '').length;
            var lines = text ? text.split('\n').length : 0;
            return t('pef-oled-input', '已输入') + ' ' + len + '/512 ' + t('pef-oled-chars', '字符，') + lines + '/6 ' + t('pef-oled-lines', '行');
        },

        _updateNeoPixelPresetVisibility(block) {
            var dropdown = block.querySelector('.pe-neopixel-preset');
            var helpEl = block.querySelector('.pe-neopixel-help');
            var singleEl = block.querySelector('.pe-action-value');
            var helpTextEl = block.querySelector('.pe-help-text');
            if (!dropdown) return;
            var actionType = parseInt(block.querySelector('.pe-action-type')?.value || '0');
            var targetId = block.querySelector('.pe-target-periph')?.value || '';
            var showPreset = actionType === 11 || (actionType === 10 && this._isNeoPixelTarget(targetId));
            this._setSectionVisible(dropdown, showPreset);
            this._setSectionVisible(helpEl, showPreset);
            if (showPreset) {
                var presetVal = dropdown.value;
                if (singleEl) {
                    singleEl.classList.remove('is-hidden');
                    if (presetVal && presetVal !== 'custom') {
                        singleEl.disabled = true;
                        singleEl.value = presetVal;
                    } else {
                        singleEl.disabled = false;
                    }
                }
                if (helpTextEl) helpTextEl.classList.add('is-hidden');
            } else {
                if (actionType === 21) {
                    if (singleEl) singleEl.classList.add('is-hidden');
                    if (helpTextEl) helpTextEl.classList.add('is-hidden');
                } else if (actionType === 27 || actionType === 15) {
                    if (singleEl) singleEl.classList.add('is-hidden');
                    if (helpTextEl) helpTextEl.classList.remove('is-hidden');
                } else {
                    if (singleEl) singleEl.classList.remove('is-hidden');
                    if (helpTextEl) helpTextEl.classList.remove('is-hidden');
                }
            }
        },

        _onNeoPixelPresetChange(selectEl, index) {
            var block = this._getPeriphExecBlock('periph-exec-actions', index);
            if (!block) return;
            var presetVal = selectEl.value;
            var singleEl = block.querySelector('.pe-action-value');
            var helpTextEl = block.querySelector('.pe-help-text');
            if (presetVal === 'custom' || presetVal === '') {
                if (singleEl) {
                    singleEl.classList.remove('is-hidden');
                    singleEl.disabled = false;
                    singleEl.value = '';
                }
                if (helpTextEl) helpTextEl.classList.remove('is-hidden');
            } else {
                if (singleEl) {
                    singleEl.classList.remove('is-hidden');
                    singleEl.disabled = true;
                    singleEl.value = presetVal;
                }
                if (helpTextEl) helpTextEl.classList.add('is-hidden');
            }
        },

        _initNeoPixelPresetFromData(block, actionValue, actionType) {
            var dropdown = block.querySelector('.pe-neopixel-preset');
            if (!dropdown) return;
            var simpleMap = {
                red: 'red', orange: 'orange', yellow: 'yellow', green: 'green', cyan: 'cyan', blue: 'blue',
                purple: 'purple', white: 'white', off: 'off', clear: 'off', black: 'off', rainbow: 'rainbow',
                cycle: 'rainbow', next: 'rainbow', chase: 'chase', theater_chase: 'theater_chase', strobe: 'strobe',
                twinkle: 'twinkle', fade: 'fade', breathing: 'breathing', color_wipe: 'color_wipe', fire: 'fire',
                '$value': 'platform_color'
            };
            if (actionType === 11) {
                if (!actionValue) return;
                dropdown.value = simpleMap[actionValue.toLowerCase()] || 'custom';
                return;
            }
            if (!actionValue) return;
            if (actionValue.startsWith('{')) {
                try {
                    var parsed = JSON.parse(actionValue);
                    var actionName = parsed.action || '';
                    var color = parsed.color || parsed.value || '';
                    var presetMap = {
                        'color:red': 'red', 'color:orange': 'orange', 'color:yellow': 'yellow',
                        'color:green': 'green', 'color:cyan': 'cyan', 'color:blue': 'blue',
                        'color:purple': 'purple', 'color:white': 'white', off: 'off', clear: 'off', black: 'off',
                        rainbow: 'rainbow', cycle: 'rainbow', next: 'rainbow'
                    };
                    var key = (actionName === 'off' || actionName === 'clear' || actionName === 'black')
                        ? actionName : (actionName + ':' + color);
                    var preset = presetMap[key];
                    if (color === '$value') preset = 'platform_color';
                    if (preset) dropdown.value = preset;
                    return;
                } catch (error) {
                    // Fall through to the legacy text format.
                }
            }
            var textPreset = simpleMap[actionValue.toLowerCase()];
            if (textPreset) dropdown.value = textPreset;
        },

        _collectPeriphExecEffectsAction(item, action) {
            if (action.actionType === 27) {
                action.actionValue = item.querySelector('.pe-action-value-oled')?.value || '';
                return true;
            }
            var preset = item.querySelector('.pe-neopixel-preset');
            if (action.actionType === 11) {
                if (preset && !preset.classList.contains('is-hidden') && preset.value && preset.value !== 'custom') {
                    action.actionValue = preset.value === 'platform_color' ? '$value' : preset.value;
                } else {
                    action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
                }
                return true;
            }
            if (action.actionType !== 10 || !preset) return false;
            if (!preset.classList.contains('is-hidden') && preset.value && preset.value !== 'custom') {
                if (preset.value === 'off') action.actionValue = JSON.stringify({action: 'off'});
                else if (preset.value === 'rainbow') action.actionValue = JSON.stringify({action: 'rainbow'});
                else if (preset.value === 'platform_color') action.actionValue = JSON.stringify({action: 'color', color: '$value'});
                else action.actionValue = JSON.stringify({action: 'color', color: preset.value});
            } else {
                action.actionValue = item.querySelector('.pe-action-value')?.value?.trim() || '';
            }
            return true;
        }
    });
})();
