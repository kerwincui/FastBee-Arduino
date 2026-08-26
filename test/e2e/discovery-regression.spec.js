/**
 * FastBee 设备自动发现机制回归测试（防硬编码 IP / 主-worker 不一致复发）
 *
 * 背景故障（均已实测发生）：
 *   - DHCP 租约隔夜漂移（.15/.64/.66 → .31/.14/.30），硬编码兜底 IP 全部失效导致空跑
 *   - Playwright worker 是独立进程重新扫描，设备瞬时离线使主/worker 设备列表不一致，
 *     报 "Test not found in the worker process"
 *   - 系统代理 HTTP(S)_PROXY 把局域网请求发往代理导致 request fixture 全灭
 *
 * 本 spec 通过子进程隔离环境变量做纯机制回归（大部分用例不依赖真机，
 * 依赖真实舰队的用例在无设备时自动 skip 而非失败）。
 *
 * 用法：npx playwright test test/e2e/discovery-regression.spec.js --project=chromium
 */

const { test, expect } = require('@playwright/test');
const { execFileSync } = require('child_process');
const path = require('path');

const HELPERS = path.resolve(__dirname, 'helpers', 'fastbee-helpers.js');
const DISCOVER = path.resolve(__dirname, 'helpers', 'discover-devices.js');
const CONFIG = path.resolve(__dirname, '..', '..', 'playwright.config.js');

/** 在独立子进程中执行代码，返回 stdout（env 完全受控，模拟主/worker 进程隔离） */
function runChild(code, env = {}) {
  return execFileSync(process.execPath, ['-e', code], {
    env: { ...process.env, ...env },
    timeout: 120000,
    encoding: 'utf8',
  });
}

/** 从子进程输出中提取 __RESULT__ 行的 JSON */
function extractResult(stdout) {
  const line = stdout.split('\n').find(l => l.startsWith('__RESULT__'));
  expect(line, `子进程无结果输出:\n${stdout}`).toBeTruthy();
  return JSON.parse(line.slice('__RESULT__'.length));
}

test.describe('设备自动发现机制回归', () => {
  test('DEVICES env 显式指定时直接采用且不触发扫描（快速路径）', async () => {
    const code = `
      const { resolveDevices } = require(${JSON.stringify(HELPERS)});
      const ips = resolveDevices();
      console.log('__RESULT__' + JSON.stringify({ ips }));
    `;
    const out = runChild(code, { DEVICES: '10.9.9.1, 10.9.9.2 ,' });
    const r = extractResult(out);
    // 空格/空段必须被清理；指定列表必须原样采用（不被扫描结果覆盖）
    expect(r.ips).toEqual(['10.9.9.1', '10.9.9.2']);
  });

  test('无 DEVICES 时自动发现并回写 process.env.DEVICES', async () => {
    test.setTimeout(150000);
    const code = `
      delete process.env.DEVICES;
      const { resolveDevices } = require(${JSON.stringify(HELPERS)});
      const ips = resolveDevices();
      console.log('__RESULT__' + JSON.stringify({ ips, written: process.env.DEVICES || '' }));
    `;
    const out = runChild(code, { DEVICES: '' });
    const r = extractResult(out);
    if (r.ips.length === 0) {
      test.skip(true, '舰队全部离线，无法验证真实扫描（机制本身已由 DEVICES 快速路径用例覆盖）');
      return;
    }
    // 回写必须与返回值完全一致——这是主/worker 一致性的前提
    expect(r.written).toBe(r.ips.join(','));
    // IP 形态校验（防发现脚本输出污染：MAC/日志混入等）
    for (const ip of r.ips) {
      expect(ip, `非法 IP 形态: ${ip}`).toMatch(/^\d{1,3}(\.\d{1,3}){3}$/);
    }
  });

  test('模拟 worker 进程：继承回写的 DEVICES 后列表与主进程完全一致', async () => {
    test.setTimeout(150000);
    // 第一步：主进程扫描并回写
    const mainCode = `
      delete process.env.DEVICES;
      const { resolveDevices } = require(${JSON.stringify(HELPERS)});
      const ips = resolveDevices();
      console.log('__RESULT__' + JSON.stringify({ ips, written: process.env.DEVICES || '' }));
    `;
    const main = extractResult(runChild(mainCode, { DEVICES: '' }));
    if (main.ips.length === 0) {
      test.skip(true, '舰队全部离线，无法验证主/worker 一致性');
      return;
    }
    // 第二步：worker 进程继承 DEVICES（Playwright 实际行为），必须走快速路径得到同一列表
    const workerCode = `
      const { resolveDevices } = require(${JSON.stringify(HELPERS)});
      const ips = resolveDevices();
      console.log('__RESULT__' + JSON.stringify({ ips }));
    `;
    const worker = extractResult(runChild(workerCode, { DEVICES: main.written }));
    expect(worker.ips, '主/worker 设备列表不一致会触发 "Test not found in worker"').toEqual(main.ips);
  });

  test('playwright.config.js 加载时清理系统代理并设置 NO_PROXY', async () => {
    // 用假 DEVICES 跳过真实扫描，只验证代理清理逻辑
    const code = `
      require(${JSON.stringify(CONFIG)});
      console.log('__RESULT__' + JSON.stringify({
        http: process.env.HTTP_PROXY || '',
        https: process.env.HTTPS_PROXY || '',
        httpL: process.env.http_proxy || '',
        all: process.env.ALL_PROXY || '',
        no: process.env.NO_PROXY || '',
        noL: process.env.no_proxy || '',
      }));
    `;
    const out = runChild(code, {
      DEVICES: '10.9.9.9',
      HTTP_PROXY: 'http://127.0.0.1:8888',
      HTTPS_PROXY: 'http://127.0.0.1:8888',
      http_proxy: 'http://127.0.0.1:8888',
      ALL_PROXY: 'http://127.0.0.1:8888',
    });
    const r = extractResult(out);
    expect(r.http).toBe('');
    expect(r.https).toBe('');
    expect(r.httpL).toBe('');
    expect(r.all).toBe('');
    expect(r.no).toBe('*');
    expect(r.noL).toBe('*');
  });

  test('discover-devices.js：MAC 前缀过滤与双扫合并（真实舰队）', async () => {
    test.setTimeout(150000);
    const code = `
      delete process.env.DEVICES;
      const { discoverSubnet } = require(${JSON.stringify(DISCOVER)});
      (async () => {
        const all = await discoverSubnet({ passes: 2 });
        // 用舰队中第一台的 MAC 前缀验证过滤逻辑（大小写不敏感 + 前缀匹配）
        let filtered = [];
        let prefix = '';
        if (all.length) {
          prefix = all[0].mac.slice(0, 8).toLowerCase();
          filtered = await discoverSubnet({ passes: 1, targetMac: prefix });
        }
        console.log('__RESULT__' + JSON.stringify({
          count: all.length,
          macs: all.map(d => d.mac),
          prefix, filteredCount: filtered.length,
          filteredMatch: filtered.every(d => d.mac.toUpperCase().startsWith(prefix.toUpperCase())),
          ipsSorted: JSON.stringify(all.map(d => parseInt(d.ip.split('.').pop(), 10)))
            === JSON.stringify(all.map(d => parseInt(d.ip.split('.').pop(), 10)).sort((a, b) => a - b)),
        }));
      })();
    `;
    const r = extractResult(runChild(code, { DEVICES: '' }));
    if (r.count === 0) {
      test.skip(true, '舰队全部离线，无法验证真实扫描合并逻辑');
      return;
    }
    // MAC 身份锚去重：双扫合并后不允许出现重复 MAC（两台同 IP 段也不能重复注册）
    expect(new Set(r.macs).size, '发现结果存在重复 MAC（合并去重失效）').toBe(r.count);
    // MAC 前缀过滤：小写前缀也能命中，且结果全部匹配
    expect(r.filteredCount).toBeGreaterThanOrEqual(1);
    expect(r.filteredMatch).toBe(true);
    // 结果按 IP 尾段排序（供展示与基线对比稳定）
    expect(r.ipsSorted).toBe(true);
  });
});
