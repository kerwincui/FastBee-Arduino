/**
 * FastBee 设备子网发现工具（DHCP 租约漂移免疫）
 *
 * 背景：路由器 DHCP 租约会隔夜漂移（2026-08-22 实测 .15/.64/.66 → .31/.14/.30），
 * 硬编码 IP 的验证脚本会在漂移后全部失效。设备身份以 /api/system/status 返回的
 * macAddress 为准，发现流程：并发探测子网 HTTP → 解析 status → 按 MAC 过滤。
 *
 * CLI：
 *   node test/e2e/helpers/discover-devices.js               # 列出所有 FastBee 设备
 *   node test/e2e/helpers/discover-devices.js D4:E9:F4      # 按 MAC 前缀过滤（大小写不敏感）
 *   node test/e2e/helpers/discover-devices.js --ips         # 仅输出逗号分隔 IP（供脚本/spec 同步消费）
 *   node test/e2e/helpers/discover-devices.js --single-pass # 只扫一轮（快速模式，默认双扫合并防瞬时漏检）
 *
 * 可靠性：设备 Web 服务在重启/忙碌瞬间可能丢探测请求（2026-08-22 实测单扫漏检 .31），
 * 默认双扫按 MAC 合并取并集；MAC 作为身份锚，两轮间 IP 变化也能正确去重。
 *
 * 模块：
 *   const { discoverSubnet } = require('./discover-devices');
 *   const devices = await discoverSubnet({ targetMac: 'D4:E9:F4:B1:6C:34' });
 */

const http = require('http');

/**
 * 探测单个 IP 的 /api/system/status，返回解析后的设备信息或 null。
 */
function probeDevice(ip, timeoutMs) {
  return new Promise(resolve => {
    const req = http.get(
      { host: ip, path: '/api/system/status', timeout: timeoutMs, agent: false },
      res => {
        let data = '';
        res.on('data', c => (data += c));
        res.on('end', () => {
          try {
            const j = JSON.parse(data);
            if (j && j.success && j.data && j.data.macAddress) {
              resolve({
                ip,
                mac: j.data.macAddress,
                ssid: j.data.ssid || '',
                rssi: typeof j.data.rssi === 'number' ? j.data.rssi : null,
                uptimeMs: j.data.uptime || 0,
              });
            } else {
              resolve(null);
            }
          } catch {
            resolve(null);
          }
        });
      }
    );
    req.on('error', () => resolve(null));
    req.on('timeout', () => {
      req.destroy();
      resolve(null);
    });
  });
}

/**
 * 并发扫描子网，返回所有 FastBee 设备（可按 targetMac 过滤，大小写不敏感，支持前缀）。
 * @param {object} [options]
 * @param {string} [options.subnet='192.168.5'] 子网前三段
 * @param {number} [options.timeoutMs=1500] 单请求超时
 * @param {number} [options.concurrency=48] 并发数
 * @param {number} [options.passes=2] 扫描轮数，多轮按 MAC 合并（防瞬时漏检）
 * @param {string} [options.targetMac] 目标 MAC（可前缀匹配）
 * @returns {Promise<Array<{ip:string,mac:string,ssid:string,rssi:number|null,uptimeMs:number}>>}
 */
async function discoverSubnet(options = {}) {
  const { subnet = '192.168.5', timeoutMs = 1500, concurrency = 48, passes = 2, targetMac } = options;
  const ips = [];
  for (let i = 2; i <= 254; i++) ips.push(`${subnet}.${i}`);
  const byMac = new Map(); // MAC 身份锚：多轮合并去重，后轮数据更新（IP 可能漂移）

  async function scanOnce() {
    let idx = 0;
    async function worker() {
      while (idx < ips.length) {
        const ip = ips[idx++];
        const d = await probeDevice(ip, timeoutMs);
        if (d) byMac.set(d.mac, d);
      }
    }
    await Promise.all(
      Array.from({ length: Math.min(concurrency, ips.length) }, worker)
    );
  }

  for (let p = 0; p < passes; p++) {
    await scanOnce();
  }

  const found = [...byMac.values()];
  found.sort((a, b) => {
    const n = s => parseInt(s.split('.').pop(), 10);
    return n(a.ip) - n(b.ip);
  });

  if (targetMac) {
    const t = targetMac.toUpperCase();
    return found.filter(d => d.mac.toUpperCase().startsWith(t));
  }
  return found;
}

if (require.main === module) {
  const args = process.argv.slice(2);
  const ipsOnly = args.includes('--ips');
  const singlePass = args.includes('--single-pass');
  const macFilter = args.find(a => !a.startsWith('--'));
  discoverSubnet({ targetMac: macFilter, passes: singlePass ? 1 : 2 })
    .then(devices => {
      if (devices.length === 0) {
        console.log('no FastBee devices found' + (macFilter ? ` matching ${macFilter}` : ''));
        process.exit(1);
      }
      if (ipsOnly) {
        console.log(devices.map(d => d.ip).join(','));
        return;
      }
      for (const d of devices) {
        console.log(`${d.ip}  mac=${d.mac}  ssid=${d.ssid}  rssi=${d.rssi}  uptime=${Math.round(d.uptimeMs / 1000)}s`);
      }
    })
    .catch(err => {
      console.error('discovery failed:', err.message);
      process.exit(1);
    });
}

module.exports = { discoverSubnet, probeDevice };
