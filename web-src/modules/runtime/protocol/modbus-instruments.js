/* NC03: reviewed read-only instrument plans become disabled local drafts. */
(function() {
    'use strict';
    const voltage = {
        type: 'fastbee-modbus-instrument', version: 1, templateId: 'eastron-sdm120-voltage-v1',
        model: 'Eastron SDM120-Modbus', revision: 'Modbus implementation V2.4', hardwareVerified: false,
        source: 'https://www.eastroneurope.com/images/uploads/products/protocol/SDM120-MODBUS_Protocol.pdf',
        sourcePages: '1,3,5', name: 'SDM120 volts',
        read: {functionCode: 4, startAddress: 0, quantity: 2,
            mappings: [{regOffset: 0, dataType: 4, wordOrder: 0, scaleFactor: 1, decimalPlaces: 1, sensorId: 'voltage', unit: 'V'}]}
    };
    const fail = message => { throw new Error(message); };
    const integer = (value, low, high) => Number.isInteger(value) && value >= low && value <= high;
    const text = (value, maximum) => typeof value === 'string' && value.trim() &&
        new TextEncoder().encode(value).length <= maximum && !/[\x00-\x1f]/.test(value);
    const keys = (value, allowed) => value && typeof value === 'object' && !Array.isArray(value) &&
        Object.keys(value).every(key => allowed.includes(key));
    Object.assign(AppState, {
        openModbusInstrument() {
            if (!this.guardDeveloperModeAction()) return;
            const opener = document.getElementById('mb-template-open');
            if (!opener) return;
            const panel = document.createElement('details');
            panel.open = true;
            panel.innerHTML = '<summary>导入只读仪表模板</summary>' +
                '<p class="field-hint">先在外设配置核对UART波特率、校验位及停止位。模板只添加禁用草稿，保存和启用仍需分别操作。</p>' +
                '<label for="mb-template-kind">模板</label>' +
                '<select id="mb-template-kind"><option value="sdm120">SDM120-Modbus 电压（资料V2.4）</option><option value="file">导入模板文件</option></select>' +
                '<label for="mb-template-file">版本1仪表模板</label><input id="mb-template-file" type="file" accept=".json,application/json">' +
                '<label for="mb-template-address">实际从站地址</label><input id="mb-template-address" type="number" min="1" max="247" step="1" placeholder="1～247">' +
                '<button type="button" class="fb-btn fb-btn-sm" data-action="previewModbusInstrument">预览</button>' +
                '<p id="mb-template-preview" class="field-hint" role="status" aria-live="polite"></p>' +
                '<label class="fb-checkbox"><input id="mb-template-confirm" type="checkbox"><span>已核对确切型号、资料版本、串口参数及从站地址</span></label>' +
                '<button id="mb-template-apply" type="button" class="fb-btn fb-btn-sm" data-action="applyModbusInstrument" disabled>添加禁用草稿</button>';
            opener.replaceWith(panel);
            this.applyDeveloperModeState(panel);
            panel.querySelector('summary').focus();
        },
        _buildModbusInstrumentDraft(plan, address) {
            if (!keys(plan, ['type','version','templateId','model','revision','source','sourcePages','hardwareVerified','name','read']) ||
                plan.type !== 'fastbee-modbus-instrument' || plan.version !== 1 || plan.hardwareVerified !== false ||
                !text(plan.templateId, 64) || !text(plan.model, 96) || !text(plan.revision, 96) ||
                !text(plan.sourcePages, 32) || !text(plan.name, 15) || !text(plan.source, 256)) fail('模板身份/版本或资料来源不完整');
            let source;
            try { source = new URL(plan.source); } catch (_) { fail('资料链接无效'); }
            if (source.protocol !== 'https:' || source.username || source.password) fail('资料来源必须是无凭据的 HTTPS 链接');
            const read = plan.read;
            if (!keys(read, ['functionCode','startAddress','quantity','mappings']) || ![3,4].includes(read.functionCode) ||
                !integer(address, 1, 247) || !integer(read.startAddress, 0, 65535) || !integer(read.quantity, 1, 32) ||
                read.startAddress + read.quantity > 65536 || !Array.isArray(read.mappings) ||
                read.mappings.length < 1 || read.mappings.length > 8) fail('只读模板仅接受 FC03/04、1～32寄存器和1～8映射');
            const ids = new Set();
            const mappings = read.mappings.map(m => {
                if (!keys(m, ['regOffset','dataType','wordOrder','scaleFactor','decimalPlaces','sensorId','unit']) ||
                    !integer(m.regOffset, 0, 31) || !integer(m.dataType, 0, 4) || !integer(m.wordOrder, 0, 1) ||
                    !integer(m.decimalPlaces, 0, 6) || typeof m.scaleFactor !== 'number' ||
                    window.decodeModbusMapping(Array(read.quantity).fill(0), m) === null ||
                    !text(m.sensorId, 15) || !/^[A-Za-z_][A-Za-z0-9_]*$/.test(m.sensorId) || ids.has(m.sensorId) ||
                    typeof m.unit !== 'string' || new TextEncoder().encode(m.unit).length > 7 || /[\x00-\x1f]/.test(m.unit))
                    fail('映射类型/字序/倍率/范围/单位或字段名无效');
                ids.add(m.sensorId);
                return {...m};
            });
            return {name: plan.name, slaveAddress: address, functionCode: read.functionCode,
                startAddress: read.startAddress, quantity: read.quantity, pollInterval: 30,
                enabled: false, mappings};
        },
        _modbusInstrumentInputs() {
            return JSON.stringify({tasks: this._masterTasks, devices: this._modbusDevices,
                protocol: this._protocolConfig,
                fields: ['mb-template-kind','mb-template-address','rtu-peripheral-id','rtu-de-pin','rtu-transfer-type']
                    .map(id => document.getElementById(id)?.value)});
        },
        async previewModbusInstrument() {
            if (!this.guardDeveloperModeAction()) return;
            const view = document.getElementById('mb-template-preview');
            const apply = document.getElementById('mb-template-apply');
            const confirm = document.getElementById('mb-template-confirm');
            if (!view || !apply || !confirm) return;
            this._mbInstrumentReview = null; apply.disabled = true; confirm.checked = false;
            const nonce = (this._mbInstrumentNonce || 0) + 1; this._mbInstrumentNonce = nonce;
            const before = this._modbusInstrumentInputs();
            const kind = document.getElementById('mb-template-kind').value;
            const file = document.getElementById('mb-template-file').files?.[0];
            try {
                let plan = voltage;
                if (kind === 'file') {
                    if (!file || file.size < 2 || file.size > 16384) fail('请选择最多16KB的仪表模板 JSON 文件');
                    plan = JSON.parse(await file.text());
                } else if (kind !== 'sdm120') fail('请选择仪表模板');
                if (nonce !== this._mbInstrumentNonce || document.getElementById('mb-template-preview') !== view ||
                    before !== this._modbusInstrumentInputs() ||
                    (kind === 'file' && document.getElementById('mb-template-file').files?.[0] !== file)) fail('配置已变化，请重新预览');
                if (!document.getElementById('rtu-peripheral-id').value) fail('请先选择UART外设并核对串口参数');
                if (document.getElementById('rtu-transfer-type').value !== '0') fail('仪表工程量模板需要 JSON 传输模式');
                const address = Number(document.getElementById('mb-template-address').value);
                const draft = this._buildModbusInstrumentDraft(plan, address);
                const current = this._masterTasks || [];
                if (current.length >= 8) fail('最多8个采集任务，请先处理现有任务');
                const existing = new Set(current.flatMap(task => (task.mappings || []).map(m => m.sensorId)));
                if (draft.mappings.some(m => existing.has(m.sensorId))) fail('工程量字段已被其它任务使用，请使用不同字段名的模板');
                view.textContent = `${plan.model} / ${plan.revision}；${kind === 'sdm120' ? '厂商资料已核对' : '外部模板，来源声明须人工核对'}，实物未验证。资料 ${plan.source}（页${plan.sourcePages}）。从站${address} FC${draft.functionCode} 零基地址${draft.startAddress} 数量${draft.quantity}。` +
                    draft.mappings.map(m => `${m.sensorId}=${['uint16','int16','uint32','int32','float32'][m.dataType]} @${m.regOffset} ×${m.scaleFactor} ${m.unit} ${m.wordOrder ? '低字在前' : '高字在前'}`).join('；');
                this._mbInstrumentReview = {before, draft, kind, file, view};
                apply.disabled = false;
            } catch (error) {
                if (nonce === this._mbInstrumentNonce && document.getElementById('mb-template-preview') === view) view.textContent = error.message;
            }
        },
        applyModbusInstrument() {
            if (!this.guardDeveloperModeAction()) return false;
            const review = this._mbInstrumentReview;
            try {
                if (!review || document.getElementById('mb-template-preview') !== review.view ||
                    !document.getElementById('mb-template-confirm')?.checked || review.before !== this._modbusInstrumentInputs() ||
                    (review.kind === 'file' && document.getElementById('mb-template-file').files?.[0] !== review.file))
                    fail('请重新预览，并核对型号/资料版本、UART格式和现场从站地址');
                this._masterTasks = [...(this._masterTasks || []), JSON.parse(JSON.stringify(review.draft))];
                this._mbInstrumentReview = null;
                document.getElementById('mb-template-apply').disabled = true;
                document.getElementById('mb-template-confirm').checked = false;
                review.view.textContent = '已添加禁用草稿；检查字段映射后使用页面保存。尚未保存、轮询或启用。';
                this._renderAllDevices();
                return true;
            } catch (error) { Notification.warning(error.message, '仪表模板'); return false; }
        }
    });
})();
