"""Run the production feature page in Node without network or shared assets."""
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]

SCRIPT = r'''
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');
const elements = {
  'ram-free8': {}, 'ram-largest8': {}, 'ram-active': {},
  'features-save-btn': {}, 'features-list': { innerHTML: '' }
};
let moduleApi;
let response;
const escapeHtml = value => String(value).replace(/[&<>"']/g, c =>
  ({'&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;'}[c]));
const context = {
  AppState: { registerModule(name, api) {
    assert.equal(name, 'feature-config'); moduleApi = api;
  } },
  escapeHtml, window: {},
  document: { getElementById: id => elements[id] || null },
  apiGet(url) { assert.equal(url, '/api/features/status'); return Promise.resolve(response); }
};
vm.runInNewContext(fs.readFileSync(process.argv[1], 'utf8'), context);
const base = {key:'sample', name:'Sample', group:'system', compiled:true,
  userEnabled:true, active:true, budgetPassed:true, admissionState:'admitted',
  activeMeaning:'admission_only', instanceState:'unobserved', ramB:1024, flashKB:2};
function row(change = {}, children = []) {
  const f = {...base, ...change};
  moduleApi._featureState = [f, ...children];
  return moduleApi.buildFeatureRow(f);
}
const cases = {
  admission() {
    assert(row().includes('已准入 · 实例状态未确认'));
    assert(!row().includes('运行中'));
    assert(row({onDemand:true}).includes('已准入 · 按需使用'));
    assert(row({userEnabled:false, active:false}).includes('用户未启用'));
    assert(!row({userEnabled:false, active:false}).includes('已停用'));
    assert(row({active:false, admissionState:undefined}).includes('启用状态待确认'));
  },
  reasons() {
    const labels = {waiting_for_network:'等待网络就绪', dependency_blocked:'依赖功能受限',
      dependency_missing:'依赖功能缺失', dependency_cycle:'依赖关系异常',
      budget_denied:'内存预算不足', runtime_disabled:'运行受限',
      restart_required:'重启后启用', user_disabled:'未获准启用'};
    for (const [admissionState, label] of Object.entries(labels))
      assert(row({active:false, admissionState}).includes(label), admissionState);
    assert(!row({active:false, admissionState:'constructor'}).includes('function Object'));
  },
  preview() {
    assert(row({userEnabled:true, savedUserEnabled:false, active:false}).includes('待保存 · 启用'));
    assert(row({userEnabled:false, savedUserEnabled:true}).includes('待保存 · 停用'));
    assert(row({userEnabled:true, savedUserEnabled:true, requiresRestart:true, bootEnabled:false}).includes('重启后启用'));
    assert(row({userEnabled:false, savedUserEnabled:false, requiresRestart:true, bootEnabled:true}).includes('重启后停用'));
  },
  child() {
    const child = {key:'child', name:null, dependsOn:'sample', compiled:true, userEnabled:true,
      active:false, runtimeDisabled:true, runtimeDisableReason:'child limited'};
    assert(row({}, [child]).includes('附属能力受限'));
    assert(row({}, [child]).includes('child limited'));
    assert(row({}, [{...child, compiled:false}]).includes('未编译'));
    assert.equal(moduleApi.buildFeatureRow(child), '');
  },
  escape() {
    const reason = '<img src=x onerror=alert(1)>"';
    const html = row({runtimeDisabled:true, runtimeDisableReason:reason});
    assert(html.includes('&lt;img'));
    assert(!html.includes('<img'));
    assert(html.includes('&quot;'));
    assert(html.includes('运行受限'));
  },
  estimates() {
    moduleApi.updateRamMonitor({free8KB:0, largest8KB:32, activeRamB:4096});
    assert.equal(elements['ram-free8'].textContent, '0KB');
    assert.equal(elements['ram-largest8'].textContent, '32KB');
    assert.equal(elements['ram-active'].textContent, '4.0KB');
    assert(elements['ram-active'].title.includes('静态 RAM 估算'));
    moduleApi._featureState = [{...base}];
    const html = moduleApi.renderGroupTables(moduleApi._featureState);
    assert(html.includes('Flash 估算') && html.includes('RAM 估算'));
    assert(html.includes('不是实测占用'));
  },
  instances() {
    const known = {instanceScope:'driver_initialization', instanceObserved:true,
      instanceState:'initialized', initializedInstances:2};
    let html = row(known);
    assert(html.includes('驱动已初始化 × 2') && !html.includes('已准入 · 实例状态未确认'));
    assert(html.includes('不等于物理工作正常') && html.includes('不代表内存已释放'));
    assert(row({...known, onDemand:true}).includes('已准入 · 按需使用'));
    assert(row({...known, userEnabled:false, active:false}).includes('用户未启用'));
    assert(row({...known, userEnabled:false, active:false}).includes('驱动已初始化 × 2'));
    assert(row({...known, savedUserEnabled:false}).includes('待保存 · 启用'));
    assert(row({...known, requiresRestart:true, bootEnabled:false}).includes('重启后启用'));
    assert(row({...known, active:false, runtimeDisabled:true}).includes('运行受限'));
    assert(row({...known, instanceState:'not_initialized', initializedInstances:0}).includes('驱动未初始化'));
    assert(row({...known, instanceState:'cleanup_pending'}).includes('实例清理待重试'));
    assert(row({...known, instanceState:'busy', instanceObserved:false}).includes('实例观测忙'));
    assert(!row({...known, compiled:false}).includes('驱动已初始化'));
  },
  invalid_instances() {
    const known = {instanceScope:'driver_initialization', instanceObserved:true,
      instanceState:'initialized', initializedInstances:2};
    for (const count of [undefined,null,0,-1,1.5,65536,'2','<img src=x>',NaN,Infinity]) {
      const html = row({...known, initializedInstances:count});
      assert(!html.includes('驱动已初始化') && html.includes('实例状态未确认'));
      assert(!html.includes('<img'));
    }
    for (const change of [{instanceScope:'physical_health'}, {instanceObserved:'true'},
      {instanceState:'__proto__'}, {instanceState:'constructor'}, {instanceState:'<img src=x>'},
      {instanceState:'not_initialized', initializedInstances:1}]) {
      const html = row({...known, ...change});
      assert(html.includes('实例状态未确认') && !html.includes('<img'));
      assert(!html.includes('驱动已初始化') && !html.includes('驱动未初始化'));
    }
  },
  lifecycle() {
    const service = {instanceScope:'service_lifecycle', instanceObserved:true};
    for (const [instanceState,label] of Object.entries({allocated:'实例已分配', running:'实例运行中',
      stopped:'实例已停止', retiring:'实例退出中'})) {
      const value = {...service, instanceState};
      const html = row(value);
      assert(html.includes(label) && !html.includes('驱动已初始化'));
      assert(!html.includes('已准入 · 实例状态未确认'));
      assert(html.includes('已停止仍可保留资源') && html.includes('不是实测占用'));
      assert(row({...value, userEnabled:false, active:false}).includes('用户未启用'));
      assert(row({...value, userEnabled:false, active:false}).includes(label));
      assert(row({...value, savedUserEnabled:false}).includes('待保存 · 启用'));
      assert(row({...value, requiresRestart:true, bootEnabled:false}).includes('重启后启用'));
      assert(row({...value, active:false, runtimeDisabled:true}).includes('运行受限'));
      assert(!row({...value, compiled:false}).includes(label));
    }
  },
  invalid_lifecycle() {
    const service = {instanceScope:'service_lifecycle', instanceObserved:true, instanceState:'running'};
    for (const change of [{instanceScope:'driver_initialization'}, {instanceObserved:false},
      {instanceObserved:'true'}, {instanceState:'constructor'}, {instanceState:'__proto__'},
      {instanceState:'<img src=x>'}, ...[0,1,null,'1'].map(initializedInstances=>({initializedInstances}))]) {
      const html = row({...service,...change});
      assert(html.includes('实例状态未确认') && !html.includes('实例运行中'));
      assert(!html.includes('<img') && !html.includes('function Object'));
    }
    assert(!row({...service, instanceState:'initialized', initializedInstances:1}).includes('驱动已初始化'));
  },
  unknown_instances() {
    const unknown = {instanceScope:'service_lifecycle', instanceObserved:false, instanceState:'unobserved'};
    for (const [instanceReason,label] of Object.entries({no_dedicated_instance:'无独立实例',
      shared_execution_owner:'共用执行服务', observer_not_registered:'观察者尚未建立',
      owner_snapshot_unavailable:'服务尚未提供快照', synchronization_unavailable:'同步条件未就绪',
      backend_unavailable:'后端不可用', invalid_snapshot:'快照不一致'})) {
      const html = row({...unknown,instanceReason,initializedInstances:8});
      assert(html.includes(label) && html.includes('实例状态未确认'));
      assert(!html.includes('× 8') && !html.includes('实例已停止'));
    }
    for (const instanceReason of ['<img src=x onerror=alert(1)>"','constructor','__proto__']) {
      const html = row({...unknown,instanceReason});
      assert(!html.includes('<img') && !html.includes('function Object'));
    }
    const busy = row({...unknown,instanceState:'busy',instanceReason:'observation_busy',initializedInstances:8});
    assert(busy.includes('实例观测忙') && !busy.includes('× 8'));
  },
  static_and_execution_scope() {
    const staticResource = {instanceScope:'static_resource', instanceObserved:false,
      instanceState:'unobserved', instanceReason:'no_dedicated_instance'};
    assert(row(staticResource).includes('静态资源 · 无独立实例'));
    assert(!row({...staticResource, instanceObserved:true}).includes('静态资源 · 无独立实例'));
    assert(!row({...staticResource, initializedInstances:0}).includes('静态资源 · 无独立实例'));
    const execution = {instanceScope:'execution_activity', instanceObserved:true};
    assert(row({...execution, instanceState:'running'}).includes('命令正在执行'));
    assert(row({...execution, instanceState:'idle'}).includes('当前无命令执行'));
    assert(!row({...execution, instanceState:'stopped'}).includes('实例已停止'));
    assert(!row({...execution, instanceState:'running', initializedInstances:1}).includes('命令正在执行'));
    assert(row({...execution, instanceState:'running', savedUserEnabled:false}).includes('待保存 · 启用'));
  },
  optional_dependency_keeps_firmware_ota_available() {
    const child = {key:'otaFs',name:null,dependsOn:'ota',compiled:false,userEnabled:false,
      bundleRequired:false,instanceReason:'implementation_missing'};
    const html = row({key:'ota'}, [child]);
    assert(!html.includes('data-feature-locked') && html.includes(' checked'));
    moduleApi.renderFeatureList = () => {};
    moduleApi.onFeatureToggle('ota', false);
    moduleApi.onFeatureToggle('ota', true);
    assert.equal(child.userEnabled, false);
    assert(row({key:'logViewer'}, [{...child,key:'fileLogging',dependsOn:'logViewer',bundleRequired:true}]).includes('未编译'));
  },
  owner_scope() {
    const scope = {instanceScope:'owner_lifetime', instanceObserved:true};
    for (const [state, label] of Object.entries({allocated:'管理器已创建', retiring:'管理器销毁中', owner_absent:'管理器未创建'})) {
      const html = row({...scope, instanceState:state});
      assert(html.includes(label) && html.includes('管理器创建不代表服务运行'));
      assert(!html.includes('实例运行中') && !html.includes('驱动已初始化'));
      assert(!row({...scope, instanceState:state, initializedInstances:1}).includes(label));
      assert(!row({...scope, instanceState:state, instanceObserved:false}).includes(label));
    }
    for (const state of ['running', 'stopped', 'initialized', 'idle']) {
      const html = row({...scope, instanceState:state});
      assert(!html.includes('管理器已创建') && !html.includes('实例运行中') && !html.includes('实例已停止'));
    }
    assert(row({...scope,instanceState:'allocated',savedUserEnabled:false}).includes('待保存 · 启用'));
  },
  tls_scope() {
    const scope = {instanceScope:'transport_allocation', instanceObserved:true};
    for (const [state, label] of Object.entries({allocated:'TLS 对象已分配', retiring:'TLS 对象释放中', stopped:'TLS 对象未分配'})) {
      const html = row({...scope, instanceState:state});
      assert(html.includes(label) && html.includes('TLS 对象分配不证明握手或证书验证'));
      assert(!row({...scope, instanceState:state, initializedInstances:0}).includes(label));
    }
    assert(!row({...scope,instanceState:'running'}).includes('实例运行中'));
    assert(!row({...scope,instanceState:'allocated',instanceObserved:false}).includes('TLS 对象已分配'));
  },
  async load() {
    response = {success:true, data:{features:[{...base, userEnabled:false}], summary:{activeRamB:0}}};
    moduleApi.renderFeatureList = () => {};
    await moduleApi.loadFeatureStatus();
    assert.equal(moduleApi._featureState[0].savedUserEnabled, false);
    assert.equal(moduleApi._featureDirty, false);
    assert.equal(elements['features-save-btn'].disabled, true);
    moduleApi._featureState[0].userEnabled = true;
    assert.equal(moduleApi._featureState[0].savedUserEnabled, false);
    assert(moduleApi.buildFeatureRow(moduleApi._featureState[0]).includes('待保存 · 启用'));
  }
};
Promise.resolve(cases[process.argv[2]]()).then(() => console.log(process.argv[2] + ': PASS'))
  .catch(error => { console.error(error); process.exitCode = 1; });
'''


class FeatureAdmissionWebTests(unittest.TestCase):
    def run_case(self, name):
        node = shutil.which('node')
        if not node:
            self.skipTest('Node.js required')
        result = subprocess.run([node, '-e', SCRIPT,
                                 str(ROOT / 'web-src/modules/runtime/feature-config.js'), name],
                                capture_output=True, text=True, encoding='utf-8',
                                errors='replace', timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(name + ': PASS', result.stdout)

    def test_admission_never_claims_instance_running_or_released(self):
        self.run_case('admission')

    def test_all_admission_failure_reasons_render(self):
        self.run_case('reasons')

    def test_unsaved_intent_and_restart_are_separate(self):
        self.run_case('preview')

    def test_hidden_bundle_member_degradation_is_visible(self):
        self.run_case('child')

    def test_runtime_diagnostics_are_html_escaped(self):
        self.run_case('escape')

    def test_estimates_do_not_replace_measured_dram(self):
        self.run_case('estimates')

    def test_status_load_captures_saved_intent_before_editing(self):
        self.run_case('load')

    def test_instance_observation_remains_separate_from_intent_and_admission(self):
        self.run_case('instances')

    def test_invalid_instance_payloads_never_invent_known_states(self):
        self.run_case('invalid_instances')

    def test_actual_service_states_do_not_override_user_intent(self):
        self.run_case('lifecycle')

    def test_service_scope_rejects_driver_counts_and_malformed_states(self):
        self.run_case('invalid_lifecycle')

    def test_unknown_coverage_reasons_are_bounded_and_never_reuse_counts(self):
        self.run_case('unknown_instances')

    def test_static_resources_and_execution_activity_are_not_instance_lifecycles(self):
        self.run_case('static_and_execution_scope')

    def test_unsupported_optional_filesystem_ota_does_not_lock_firmware_ota(self):
        self.run_case('optional_dependency_keeps_firmware_ota_available')

    def test_manager_owners_do_not_claim_service_running_or_driver_counts(self):
        self.run_case('owner_scope')

    def test_tls_allocation_does_not_claim_a_live_authenticated_connection(self):
        self.run_case('tls_scope')


if __name__ == '__main__':
    unittest.main()
