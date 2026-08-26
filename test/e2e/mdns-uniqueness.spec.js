/**
 * FastBee mDNS 域名唯一性守护（防同名冲突复发）
 *
 * 背景：所有设备 customDomain 默认为同名 fastbee，多设备同网段时 mDNS 同名冲突
 * 导致 fastbee.local 解析随机或失败（2026-08-22 实测：两台设备同时注册 fastbee，
 * PC 端 ping fastbee.local 完全失败）。固件已内置同名预探测 + 自动后缀
 * （fastbee-2…fastbee-8，见 DNSManager.cpp），新设备加入无需手动改名；
 * 本 spec 作为守护测试防止冲突复发（探测依赖多播，弱射频设备可能漏检）。
 *
 * 检查项：
 *   1. 每台在线设备的 mdnsDomain（/api/network/status，需登录）在舰队内唯一
 *   2. 设备瞬时离线仅跳过不失败（间歇离线为射频环境问题）
 *   3. 弱信号设备（RSSI < -75）仅告警不失败
 *
 * 用法：
 *   npx playwright test test/e2e/mdns-uniqueness.spec.js --project=chromium
 *   （默认自动发现在线设备；DEVICES env 可指定）
 */

const { test, expect } = require('@playwright/test');
const http = require('http');
const { CREDENTIALS, resolveDevices } = require('./helpers/fastbee-helpers');

const devices = resolveDevices();

/** 表单登录，返回后续请求可用的 Cookie 头（失败返回 null）。 */
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

/** 探测 mDNS 域名可达性（PC 侧 .local 解析 + HTTP），重试一次，返回状态码或 0 */
function domainReachable(domain, attempts = 2) {
  return new Promise(resolve => {
    let attempt = 0;
    const tryOnce = () => {
      attempt += 1;
      const req = http.get(
        { host: `${domain}.local`, path: '/api/system/status', timeout: 8000, agent: false },
        res => {
          res.resume();
          resolve(res.statusCode);
        }
      );
      req.on('error', () => {
        if (attempt < attempts) { setTimeout(tryOnce, 2000); } else { resolve(0); }
      });
      req.on('timeout', () => {
        req.destroy();
        if (attempt < attempts) { setTimeout(tryOnce, 2000); } else { resolve(0); }
      });
    };
    tryOnce();
  });
}

// 舰队命名模式：固件自动后缀机制产出 fastbee / fastbee-2…fastbee-8，
// 弱射频设备可固定专属域名（如 fastbee-classic）；一律以 fastbee 为前缀
const DOMAIN_PATTERN = /^fastbee(-[0-9a-z]+)?$/i;

test.describe('FastBee mDNS Uniqueness Guard', () => {
  for (const device of devices) {
    test(`mdns domain unique - ${device}`, async () => {
      const cookie = await loginCookie(device);
      if (!cookie) {
        // 区分离线与凭据漂移：无需认证的 system/status 探活
        const alive = await authedGet(device, '/api/system/status', null);
        if (!alive) {
          console.log(`[${device}] SKIP: 设备离线（弱射频窗口？），不计为唯一性失败`);
          test.skip(true, `device offline: ${device}`);
          return;
        }
      }
      expect(cookie, `登录失败（凭据漂移？重刷文件系统重置 users.json）: ${device}`).toBeTruthy();

      const status = await authedGet(device, '/api/network/status', cookie);
      if (!status || !status.success) {
        // 可能测试中途掉线（弱射频窗口短）：再探活，离线则跳过
        const alive = await authedGet(device, '/api/system/status', null);
        if (!alive) {
          console.log(`[${device}] SKIP: 测试中途设备掉线，不计为唯一性失败`);
          test.skip(true, `device dropped mid-test: ${device}`);
          return;
        }
      }
      expect(status && status.success, `network/status 请求失败: ${device}`).toBeTruthy();

      // 实际结构：字段平铺在 data 下（2026-08-22 实测），兼容嵌套 network 结构
      const d = status.data || {};
      const net = d.network || {};
      const domain = d.mdnsDomain || net.mdnsDomain || '';
      const customDomain = d.customDomain || net.customDomain || '';
      const rssi = d.rssi != null ? d.rssi : (d.status || {}).rssi;
      console.log(`[${device}] customDomain=${customDomain || '-'} mdnsDomain=${domain || '-'} rssi=${rssi}`);
      expect(domain, `mdnsDomain 字段缺失（enableMDNS 被关闭？）: ${device}`).toBeTruthy();
      // 命名模式守护：域名必须以 fastbee 为前缀（自动后缀 fastbee-N 或专属域名 fastbee-xxx），
      // 防止 customDomain 被误改为任意值后与舰队命名体系脱节
      expect(domain, `mdnsDomain 不符合舰队命名模式 fastbee[-后缀]: ${device}`).toMatch(DOMAIN_PATTERN);

      // 弱信号仅告警：间歇离线是射频环境问题，不属于配置缺陷
      if (typeof rssi === 'number' && rssi < -75) {
        console.log(`[${device}] WARN: RSSI=${rssi} 弱信号，设备可能间歇离线（移近路由/换天线）`);
      }
    });
  }

  test('fleet-wide mdns domains are unique', async () => {
    test.skip(devices.length < 2, '少于 2 台在线设备，无冲突可查');
    // 串行登录每台最坏 20s（登录 10s + status 10s），默认 30s 必超时（2026-08-22 实测 flaky）
    test.setTimeout(devices.length * 20000 + 30000);
    const domains = new Map(); // mdnsDomain -> [ip]
    for (const device of devices) {
      const cookie = await loginCookie(device);
      if (!cookie) {
        console.log(`[${device}] 登录失败，跳过（可能瞬时离线）`);
        continue;
      }
      const status = await authedGet(device, '/api/network/status', cookie);
      const sd = (status && status.success && status.data) || {};
      const domain = sd.mdnsDomain || (sd.network || {}).mdnsDomain || '';
      if (!domain) continue;
      if (!domains.has(domain)) domains.set(domain, []);
      domains.get(domain).push(device);
    }
    const conflicts = [...domains.entries()].filter(([, ips]) => ips.length > 1);
    if (conflicts.length) {
      const detail = conflicts.map(([d, ips]) => `${d} -> ${ips.join(', ')}`).join('; ');
      expect(conflicts, `mDNS 同名冲突（fastbee.local 将解析错乱）：${detail}。处置：重启冲突设备触发重新探测（固件自动后缀），或手动设唯一 customDomain（POST /api/network/config）`).toEqual([]);
    }
    console.log(`mDNS 域名唯一性 OK：${[...domains.entries()].map(([d, ips]) => `${d}=${ips[0]}`).join(', ')}`);

    // PC 侧域名可达性巡检（软检查）：弱射频设备的多播通告可能瞬时丢失（已知硬件限制），
    // 重试后仍不可达仅告警不计失败；同名冲突已由上方硬断言拦截
    for (const [domain, ips] of domains.entries()) {
      const code = await domainReachable(domain);
      if (code >= 200 && code < 500) {
        console.log(`[reach] ${domain}.local -> HTTP ${code} (${ips[0]})`);
      } else {
        console.log(`[reach] WARN: ${domain}.local 不可达（弱射频多播通告丢失？重启设备重新注册）`);
      }
    }
  });
});
