/**
 * FastBee 预置外设配置守护（防 LittleFS 预置配置漂移）
 *
 * 背景：2026-08-22 C6 部署采用 data/config/peripherals.json + periph_exec.json
 * 直接写入 LittleFS 的预置配置路径（WS2812B 板载彩灯 GPIO8 + 定时变色灯规则）。
 * 该路径绕过 Web UI 导入，若 JSON 字段与固件解析逻辑脱节（如 type 编号变更、
 * params 结构变化），设备启动后外设静默失效且无明显报错。
 *
 * 检查项：
 *   1. 目标设备（MAC 匹配 PRESET_MACS）的预置外设均成功初始化（status=4 运行中）
 *   2. 目标设备的执行规则已加载且启用
 *   3. 非目标设备（旧部署、无预置配置）仅记录不失败
 *   4. 设备瞬时离线仅跳过不失败（弱射频容错）
 *
 * 用法：
 *   npx playwright test test/e2e/peripheral-preset.spec.js --project=chromium
 */

const { test, expect } = require('@playwright/test');
const http = require('http');
const { CREDENTIALS, resolveDevices } = require('./helpers/fastbee-helpers');

const devices = resolveDevices();

// 本次预置配置的目标设备 MAC（大写，不带分隔符也可）：
// C6 = WS2812B 板载彩灯 + 定时变色灯规则（2026-08-22 部署）
const PRESET_MACS = ['98:A3:16:9B:59:DC'];

/** 表单登录，返回 Cookie 头（失败返回 null）。 */
function loginCookie(ip, timeoutMs = 10000) {
  return new Promise(resolve => {
    const body = `username=${encodeURIComponent(CREDENTIALS.username)}&password=${encodeURIComponent(CREDENTIALS.password)}`;
    const req = http.request(
      {
        host: ip, path: '/api/auth/login', method: 'POST', timeout: timeoutMs, agent: false,
        headers: {
          'Content-Type': 'application/x-www-form-urlencoded',
          'Content-Length': Buffer.byteLength(body),
        },
      },
      res => {
        let data = '';
        res.on('data', c => (data += c));
        res.on('end', () => {
          try {
            const j = JSON.parse(data);
            if (!j || !j.success) return resolve(null);
            const setCookie = res.headers['set-cookie'];
            const cookies = Array.isArray(setCookie)
              ? setCookie.map(c => c.split(';')[0]).join('; ')
              : '';
            resolve(cookies || null);
          } catch {
            resolve(null);
          }
        });
      }
    );
    req.on('error', () => resolve(null));
    req.on('timeout', () => { req.destroy(); resolve(null); });
    req.write(body);
    req.end();
  });
}

/** 带 Cookie 的 GET，返回解析后的 JSON（失败返回 null）。 */
function authedGet(ip, path, cookie, timeoutMs = 10000) {
  return new Promise(resolve => {
    const req = http.get(
      { host: ip, path, timeout: timeoutMs, agent: false, headers: cookie ? { Cookie: cookie } : {} },
      res => {
        let data = '';
        res.on('data', c => (data += c));
        res.on('end', () => {
          try { resolve(JSON.parse(data)); } catch { resolve(null); }
        });
      }
    );
    req.on('error', () => resolve(null));
    req.on('timeout', () => { req.destroy(); resolve(null); });
  });
}

/** 查设备 MAC（无需登录），失败返回 null。 */
function getMac(ip) {
  return new Promise(resolve => {
    const req = http.get(
      { host: ip, path: '/api/system/status', timeout: 8000, agent: false },
      res => {
        let data = '';
        res.on('data', c => (data += c));
        res.on('end', () => {
          try { resolve(JSON.parse(data).data.macAddress || null); } catch { resolve(null); }
        });
      }
    );
    req.on('error', () => resolve(null));
    req.on('timeout', () => { req.destroy(); resolve(null); });
  });
}

test.describe('FastBee Peripheral Preset Guard', () => {
  for (const device of devices) {
    test(`preset peripherals initialized - ${device}`, async () => {
      const mac = await getMac(device);
      const isTarget = mac && PRESET_MACS.includes(mac.toUpperCase());
      if (!isTarget) {
        console.log(`[${device}] mac=${mac || '-'} 非预置目标设备，跳过强校验`);
        test.skip(true, `not a preset target: ${device}`);
        return;
      }

      const cookie = await loginCookie(device);
      if (!cookie) {
        const alive = await authedGet(device, '/api/system/status', null);
        if (!alive) {
          console.log(`[${device}] SKIP: 设备离线（弱射频窗口？）`);
          test.skip(true, `device offline: ${device}`);
          return;
        }
      }
      expect(cookie, `登录失败（凭据漂移？重刷文件系统）: ${device}`).toBeTruthy();

      const list = await authedGet(device, '/api/peripherals/list', cookie);
      if (!list || !list.success) {
        const alive = await authedGet(device, '/api/system/status', null);
        if (!alive) {
          test.skip(true, `device dropped mid-test: ${device}`);
          return;
        }
      }
      expect(list && list.success, `peripherals/list 请求失败: ${device}`).toBeTruthy();

      const periphs = list.data || [];
      console.log(`[${device}] 预置外设数量: ${periphs.length}`);
      // 每台设备至少应有预置外设（C6=rgb_led, C3=dht11/oled/led 等）
      expect(periphs.length, `无预置外设（peripherals.json 未写入或解析失败）: ${device}`).toBeGreaterThan(0);

      // 每个外设必须初始化成功（status=4 运行中；其他状态表示初始化失败）
      for (const p of periphs) {
        console.log(`[${device}]   ${p.id}(${p.typeName}) pins=${p.pins} status=${p.status}`);
        expect(p.status, `外设 ${p.id} 初始化失败（status=${p.status}，检查引脚冲突或类型编号）`).toBe(4);
      }
    });

    test(`preset exec rules loaded - ${device}`, async () => {
      const mac = await getMac(device);
      const isTarget = mac && PRESET_MACS.includes(mac.toUpperCase());
      if (!isTarget) {
        test.skip(true, `not a preset target: ${device}`);
        return;
      }

      const cookie = await loginCookie(device);
      if (!cookie) {
        const alive = await authedGet(device, '/api/system/status', null);
        if (!alive) {
          test.skip(true, `device offline: ${device}`);
          return;
        }
      }
      expect(cookie, `登录失败: ${device}`).toBeTruthy();

      const exec = await authedGet(device, '/api/periph-exec/list', cookie);
      if (!exec || !exec.success) {
        const alive = await authedGet(device, '/api/system/status', null);
        if (!alive) {
          test.skip(true, `device dropped mid-test: ${device}`);
          return;
        }
      }
      expect(exec && exec.success, `periph-exec/list 请求失败: ${device}`).toBeTruthy();

      const rules = exec.data || [];
      console.log(`[${device}] 预置执行规则数量: ${rules.length}`);
      expect(rules.length, `无预置执行规则（periph_exec.json 未写入或解析失败）: ${device}`).toBeGreaterThan(0);

      for (const r of rules) {
        console.log(`[${device}]   ${r.id}(${r.name}) enabled=${r.enabled}`);
        expect(r.enabled, `执行规则 ${r.id} 未启用（periph_exec.json enabled 字段异常）`).toBe(true);
      }
    });
  }
});
