/**
 * FastBee 稳定性测试 - 全局 Setup
 *
 * 在所有测试开始前执行一次：
 *   1. 设备健康预检（可达性 + 内存基线）
 *   2. 输出设备信息摘要（固件版本、uptime、heap、PSRAM）
 *   3. 若设备不可达且启用自动复位，尝试串口恢复
 */

import { execSync } from 'child_process';

const DEVICE_IP = process.env.DEVICE_IP || '192.168.1.1';
const DEVICE_PORT = process.env.DEVICE_PORT || '80';
const BASE_URL = `http://${DEVICE_IP}:${DEVICE_PORT}`;
const AUTO_RESET = process.env.DEVICE_AUTO_RESET === '1';
const SERIAL_PORT = process.env.DEVICE_SERIAL || '';

async function fetchJson(url: string, timeoutMs = 10_000): Promise<Record<string, unknown> | null> {
  try {
    const resp = await fetch(url, { signal: AbortSignal.timeout(timeoutMs) });
    if (!resp.ok) return null;
    return await resp.json() as Record<string, unknown>;
  } catch {
    return null;
  }
}

function serialReset(port: string): boolean {
  if (!port) return false;
  try {
    execSync(
      `python -c "import serial,time;s=serial.Serial('${port}',115200,timeout=2);s.rts=True;time.sleep(0.1);s.rts=False;time.sleep(0.1);s.close()"`,
      { timeout: 10_000, stdio: 'pipe' }
    );
    console.log(`[StabilitySetup] RTS reset pulse sent to ${port}`);
    return true;
  } catch {
    return false;
  }
}

async function waitForDevice(maxMs = 60_000): Promise<boolean> {
  const start = Date.now();
  while (Date.now() - start < maxMs) {
    const health = await fetchJson(`${BASE_URL}/api/health`, 5_000);
    if (health && health.ok) return true;
    await new Promise(r => setTimeout(r, 2_000));
  }
  return false;
}

async function getDeviceInfo(): Promise<Record<string, unknown> | null> {
  return fetchJson(`${BASE_URL}/api/system/info?probe=1`, 15_000);
}

function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(1)}MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(1)}KB`;
  return `${bytes}B`;
}

function formatUptime(ms: number): string {
  const s = Math.floor(ms / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (h > 0) return `${h}h${m}m${sec}s`;
  if (m > 0) return `${m}m${sec}s`;
  return `${sec}s`;
}

async function stabilityGlobalSetup() {
  const rounds = process.env.STABILITY_ROUNDS || '5';
  const workers = process.env.STABILITY_WORKERS || '2';

  console.log('\n╔══════════════════════════════════════════════════════╗');
  console.log('║       FastBee 长期稳定性测试 - 设备预检             ║');
  console.log('╚══════════════════════════════════════════════════════╝');
  console.log(`  设备地址    : ${BASE_URL}`);
  console.log(`  测试轮数    : ${rounds}`);
  console.log(`  并行 Workers : ${workers}`);
  console.log(`  自动复位    : ${AUTO_RESET ? '启用' : '关闭'}`);
  if (SERIAL_PORT) console.log(`  串口        : ${SERIAL_PORT}`);

  // 1. 健康预检
  console.log('\n[1/3] 设备健康预检...');
  let healthy = await waitForDevice(15_000);

  if (!healthy && AUTO_RESET && SERIAL_PORT) {
    console.log('[StabilitySetup] 设备不可达，尝试串口复位...');
    serialReset(SERIAL_PORT);
    healthy = await waitForDevice(60_000);
  }

  if (!healthy) {
    console.error('[StabilitySetup] ⚠ 设备不可达，测试可能失败');
    console.error(`  目标: ${BASE_URL}`);
    console.error('  提示: 检查网络连接或设置 DEVICE_AUTO_RESET=1 DEVICE_SERIAL=COMx');
    return; // 不阻塞，让测试自行报告失败
  }

  console.log('[StabilitySetup] ✓ 设备可达');

  // 2. 设备信息采集
  console.log('\n[2/3] 采集设备信息...');
  const info = await getDeviceInfo();
  if (info) {
    const heapFree = Number(info.heapFree || 0);
    const heapSize = Number(info.heapSize || 0);
    const psramFree = Number(info.psramFree || 0);
    const uptime = Number(info.uptime || 0);
    const firmware = info.firmwareVersion || info.version || 'unknown';

    console.log(`  固件版本    : ${firmware}`);
    console.log(`  运行时间    : ${formatUptime(uptime)}`);
    console.log(`  DRAM 空闲   : ${formatBytes(heapFree)} / ${formatBytes(heapSize)} (${heapSize > 0 ? ((heapFree / heapSize) * 100).toFixed(1) : '?'}%)`);
    if (psramFree > 0) {
      const psramSize = Number(info.psramSize || 0);
      console.log(`  PSRAM 空闲  : ${formatBytes(psramFree)} / ${formatBytes(psramSize)} (${psramSize > 0 ? ((psramFree / psramSize) * 100).toFixed(1) : '?'}%)`);
    }

    // 写入基线数据到环境变量，供测试过程对比
    process.env.STABILITY_BASELINE_HEAP = String(heapFree);
    process.env.STABILITY_BASELINE_UPTIME = String(uptime);
    if (psramFree > 0) {
      process.env.STABILITY_BASELINE_PSRAM = String(psramFree);
    }
  } else {
    console.log('[StabilitySetup] ⚠ 无法获取设备信息（API 可能受限）');
  }

  // 3. 稳定性阈值摘要
  console.log('\n[3/3] 稳定性监控指标:');
  console.log('  - 设备崩溃检测 : uptime 重置 → 记录 CRASH');
  console.log('  - 内存泄漏预警 : heap 持续下降 > 8KB → 记录 WARNING');
  console.log('  - 失败率阈值   : 单轮 > 10% → 记录 ALERT');
  console.log('');
  console.log('═══════════════════════════════════════════════════════');
  console.log('  开始稳定性测试...');
  console.log('═══════════════════════════════════════════════════════\n');
}

export default stabilityGlobalSetup;
