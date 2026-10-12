"""Prepared actual JS group dialog checks with a tiny DOM boundary."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = r'''
const assert = require('assert/strict'), fs = require('fs'), vm = require('vm');
class Element {
    constructor(tag) { this.tag = tag; this.children = []; this.handlers = {}; this.checked = false; }
    append(...values) { this.children.push(...values); }
    appendChild(value) { this.append(value); }
    addEventListener(name, fn) { this.handlers[name] = fn; }
    setAttribute() {}
    showModal() { this.shown = true; }
    remove() { this.removed = true; }
}
const body = new Element('body');
const app = {};
const context = vm.createContext({
    AppState: {registerModule: (name, methods) => { Object.assign(app, methods); }}, window: {},
    setTimeout, clearTimeout,
    document: {body, createElement: (tag) => new Element(tag), createTextNode: (text) => ({text})}
});
vm.runInContext(fs.readFileSync('web-src/modules/runtime/peripherals.js', 'utf8'), context);
context.ModuleLoader = {loadModule(name, ready) {
    assert.equal(name, 'peripheral-io');
    vm.runInContext(fs.readFileSync('web-src/modules/runtime/peripheral-io.js', 'utf8'), context);
    ready();
}};
const child = (id, channel, initialState = 0, invertLogic = false) => ({id, name: id, type: 12, enabled: false,
    params: {providerId: 'mcp', channel, initialState, invertLogic}});
const preview = {id: 'mcp', groupToken: '1'.repeat(48),
    provider: {id: 'mcp', enabled: false, pins: [21, 22], params: {address: 32, busPeripheralId: ''}},
    reset: {id: 'reset', enabled: true, pins: [18], params: {nativeControlEligible: true}},
    channels: [child('b7', 15, 1, true), child('a7', 7), child('optional', 2, 1)]};
let requests = 0;
const getter = async (url, args) => {
    requests++; assert.equal(url, '/api/peripherals/status'); assert.equal(args.id, 'mcp'); assert.equal(args.groupPreview, 1);
    return {success: true, data: preview};
};
function nodes(root, test) { return [root, ...(root.children || []).flatMap((n) => nodes(n, test))].filter(test); }
async function dialog() {
    const result = app._selectMcpGroupChannels('mcp', getter);
    await new Promise(setImmediate);
    const modal = body.children.at(-1);
    assert(modal.shown && requests === 1);
    return {result, modal, form: nodes(modal, (n) => n.tag === 'form')[0], inputs: nodes(modal, (n) => n.tag === 'input')};
}
async function rejected(message) {
    await assert.rejects(app._selectMcpGroupChannels('mcp', getter), (err) => err.message.includes(message));
    assert.equal(body.children.length, 0);
}
const cases = {
    async explicit_selection() {
        const d = await dialog(); assert(d.inputs.every((n) => !n.checked));
        d.inputs[1].checked = true; d.inputs[2].checked = true;
        d.form.handlers.submit({preventDefault(){}});
        const selected = await d.result;
        assert.deepEqual(Array.from(selected.channelIds), ['a7', 'b7']);
        assert.equal(selected.groupToken, preview.groupToken); assert(d.modal.removed);
    },
    async required_outputs() {
        const d = await dialog(); d.inputs[0].checked = true;
        d.form.handlers.submit({preventDefault(){}}); assert(!d.modal.removed);
        assert(nodes(d.modal, (n) => n.textContent?.includes('请核对并选中必需')).length);
        d.modal.handlers.cancel({preventDefault(){}}); assert.equal(await d.result, null);
    },
    async wiring_and_inversion() {
        const d = await dialog();
        const text = nodes(d.modal, () => true).map((n) => n.text || n.textContent || '').join('\n');
        assert(text.includes('GPIO 18') && text.includes('SDA/SCL 21/22'));
        assert(text.includes('P15 · b7 (b7) · 输出初始物理电平 低'));
        d.modal.handlers.cancel({preventDefault(){}}); await d.result;
    },
    async bad_token() { preview.groupToken = ''; await rejected('整组预览读取失败'); },
    async wrong_provider() { preview.id = 'different'; await rejected('整组预览读取失败'); },
    async enabled_channel() { preview.channels[0].enabled = true; await rejected('禁用状态'); },
    async duplicate_channel() { preview.channels[2].params.channel = 7; await rejected('配置不完整或重复'); },
    async input_b7() { preview.channels[0].type = 11; await rejected('配置不完整或重复'); },
    async unknown_output() { delete preview.channels[0].params.invertLogic; await rejected('配置不完整或重复'); },
    async reset_not_ready() { preview.reset.enabled = false; await rejected('专用 RESET'); },
    async bus_not_ready() { preview.provider.params.busPeripheralId = 'bus'; await rejected('引用总线'); }
};
cases[process.argv[1]]().catch((error) => { console.error(error); process.exitCode = 1; });
'''


class McpGroupWebTests(unittest.TestCase):
    def check(self, case):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node required')
        result = subprocess.run([node, '-e', SCRIPT, case], cwd=ROOT, capture_output=True,
                                text=True, encoding='utf-8', errors='replace', timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ('explicit_selection', 'required_outputs', 'wiring_and_inversion', 'bad_token',
              'wrong_provider', 'enabled_channel', 'duplicate_channel', 'input_b7',
              'unknown_output', 'reset_not_ready', 'bus_not_ready'):
    setattr(McpGroupWebTests, 'test_' + _case, lambda self, case=_case: self.check(case))

if __name__ == '__main__':
    unittest.main()
