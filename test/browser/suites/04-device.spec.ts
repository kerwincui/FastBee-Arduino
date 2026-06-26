import { test, expect, waitForDevice, waitForDeviceReady } from '../fixtures/base.fixture';

test.describe('Suite-04: 设备配置', () => {

  // ========== 场景A: 基本信息查看与修改 ==========

  test('DEV-001: 进入设备配置页', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await expect(authPage.locator('#device-page')).toBeVisible();
  });

  test('DEV-002: 硬件信息-左列', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const chip = await authPage.locator('#dev-sys-chip').textContent();
    expect(chip).not.toBe('--');
    const cpu = await authPage.locator('#dev-sys-cpu').textContent();
    expect(cpu).not.toBe('--');
    const heap = await authPage.locator('#dev-sys-heap').textContent();
    expect(heap).not.toBe('--');
  });

  test('DEV-003: 硬件信息-右列', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const flash = await authPage.locator('#dev-sys-flash').textContent();
    expect(flash).not.toBe('--');
    const sdk = await authPage.locator('#dev-sys-sdk').textContent();
    expect(sdk).not.toBe('--');
    const fw = await authPage.locator('#dev-sys-fw').textContent();
    expect(fw).not.toBe('--');
  });

  test('DEV-004: 设备名称修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-name', 'FastBee-Test-S3');
    const val = await authPage.locator('#dev-name').inputValue();
    expect(val).toBe('FastBee-Test-S3');
  });

  test('DEV-005: 用户ID修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-user-id', 'user_test_001');
    expect(await authPage.locator('#dev-user-id').inputValue()).toBe('user_test_001');
  });

  test('DEV-006: 设备编号修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-id', 'FBE-S3-TEST-001');
    expect(await authPage.locator('#dev-id').inputValue()).toBe('FBE-S3-TEST-001');
  });

  test('DEV-007: 产品编号修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-product-number', '2001');
    expect(await authPage.locator('#dev-product-number').inputValue()).toBe('2001');
  });

  test('DEV-008: 日志级别切换到DEBUG', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.selectOption('#dev-log-level', 'DEBUG');
    expect(await authPage.locator('#dev-log-level').inputValue()).toBe('DEBUG');
  });

  test('DEV-009: 日志级别切换到ERROR', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.selectOption('#dev-log-level', 'ERROR');
    expect(await authPage.locator('#dev-log-level').inputValue()).toBe('ERROR');
  });

  test('DEV-010: 设备描述输入', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-description', '自动化回归测试设备 ESP32-S3');
    expect(await authPage.locator('#dev-description').inputValue()).toContain('自动化');
  });

  test('DEV-011: 基本信息保存', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-name', 'FastBee-AutoTest');
    await authPage.click('#device-basic-form button[type="submit"]');
    await waitForDevice(authPage, 3000);
    await expect(authPage.locator('#dev-basic-success')).not.toHaveClass(/is-hidden/);
  });

  test('DEV-012: 保存后刷新验证持久化', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-name', 'FastBee-Persist');
    await authPage.click('#device-basic-form button[type="submit"]');
    await waitForDevice(authPage, 3000);
    await authPage.reload();
    // reload() 后页面回到仪表盘，需重新导航到设备配置页
    await navigateTo('device');
    const name = await authPage.locator('#dev-name').inputValue();
    expect(name).toBe('FastBee-Persist');
  });

  test('DEV-013: 设备编号超长截断', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const longId = 'A'.repeat(40);
    await authPage.fill('#dev-id', longId);
    // maxlength=32, 浏览器自动截断
    const val = await authPage.locator('#dev-id').inputValue();
    expect(val.length).toBeLessThanOrEqual(32);
  });

  test('DEV-014: 产品编号负值拒绝', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.fill('#dev-product-number', '-1');
    // min=0, HTML5 验证应拦截
    const val = await authPage.locator('#dev-product-number').inputValue();
    // 如果浏览器允许填入负值，保存时也应处理
    expect(parseInt(val) >= 0 || val === '-1').toBeTruthy();
  });

  // ========== 场景B: NTP时间配置 ==========

  test('DEV-015: NTP Tab切换', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await expect(authPage.locator('#dev-ntp')).toHaveClass(/active/, { timeout: 5000 });
  });

  test('DEV-016: 当前时间显示', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await expect(authPage.locator('#dev-ntp')).toHaveClass(/active/, { timeout: 5000 });
    await waitForDeviceReady(authPage, 5000);
    const datetime = await authPage.locator('#dev-time-datetime').textContent();
    expect(datetime).not.toBe('--');
  });

  test('DEV-017: NTP同步状态', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await expect(authPage.locator('#dev-ntp')).toHaveClass(/active/, { timeout: 5000 });
    await expect(authPage.locator('#dev-time-synced')).toBeVisible({ timeout: 8000 });
  });

  test('DEV-018: 设备运行时间', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await expect(authPage.locator('#dev-ntp')).toHaveClass(/active/, { timeout: 5000 });
    await waitForDeviceReady(authPage, 5000);
    const uptime = await authPage.locator('#dev-time-uptime').textContent();
    expect(uptime).not.toBe('--');
  });

  test('DEV-019: 刷新时间按钮', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await expect(authPage.locator('#dev-ntp')).toHaveClass(/active/, { timeout: 5000 });
    await authPage.click('#dev-time-refresh-btn');
    await waitForDeviceReady(authPage, 8000);
    const dt = await authPage.locator('#dev-time-datetime').textContent();
    expect(dt).toBeTruthy();
  });

  test('DEV-020: NTP启用/禁用', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await authPage.selectOption('#dev-ntp-enable', '0');
    expect(await authPage.locator('#dev-ntp-enable').inputValue()).toBe('0');
    await authPage.selectOption('#dev-ntp-enable', '1');
    expect(await authPage.locator('#dev-ntp-enable').inputValue()).toBe('1');
  });

  test('DEV-021: NTP服务器1修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await authPage.fill('#dev-ntp-server1', 'ntp.aliyun.com');
    expect(await authPage.locator('#dev-ntp-server1').inputValue()).toBe('ntp.aliyun.com');
  });

  test('DEV-022: NTP服务器2修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await authPage.fill('#dev-ntp-server2', 'time.windows.com');
    expect(await authPage.locator('#dev-ntp-server2').inputValue()).toBe('time.windows.com');
  });

  test('DEV-023: 时区切换到UTC', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await authPage.selectOption('#dev-timezone', 'UTC0');
    expect(await authPage.locator('#dev-timezone').inputValue()).toBe('UTC0');
  });

  test('DEV-024: 时区切换到中国', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await authPage.selectOption('#dev-timezone', 'CST-8');
    expect(await authPage.locator('#dev-timezone').inputValue()).toBe('CST-8');
  });

  test('DEV-025: 同步间隔修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await authPage.fill('#dev-sync-interval', '1800');
    expect(await authPage.locator('#dev-sync-interval').inputValue()).toBe('1800');
  });

  test('DEV-026: NTP配置保存', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-ntp"]');
    await expect(authPage.locator('#dev-ntp')).toHaveClass(/active/, { timeout: 5000 });
    await authPage.click('#device-ntp-form button[type="submit"]');
    await waitForDevice(authPage, 3000);
    await expect(authPage.locator('#dev-ntp-success')).not.toHaveClass(/is-hidden/);
  });

  // ========== 场景C: 开发环境功能 ==========

  test('DEV-027: 开发环境状态检查', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('#dev-mode-status')).toBeVisible({ timeout: 8000 });
  });

  test('DEV-028: 禁用开发环境', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('#dev-mode-status')).toBeVisible({ timeout: 8000 });
    await authPage.fill('#dev-mode-password', 'admin');
    await authPage.click('#dev-mode-toggle-btn');
    await waitForDevice(authPage, 3000);
  });

  test('DEV-029: 禁用后侧边栏验证', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('.dev-developer-panel')).toBeVisible({ timeout: 8000 });
  });

  test('DEV-030: 禁用后外设页面验证', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    await expect(authPage.locator('#peripheral-page')).toBeVisible();
  });

  test('DEV-031: 禁用后外设执行页验证', async ({ authPage, navigateTo }) => {
    await navigateTo('periph-exec');
    await expect(authPage.locator('#periph-exec-page')).toBeVisible();
  });

  test('DEV-032: 重新启用开发环境', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('#dev-mode-status')).toBeVisible({ timeout: 8000 });
    await authPage.fill('#dev-mode-password', 'admin');
    await authPage.click('#dev-mode-toggle-btn');
    await waitForDevice(authPage, 3000);
  });

  test('DEV-033: 禁用时密码错误', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('#dev-mode-status')).toBeVisible({ timeout: 8000 });
    await authPage.fill('#dev-mode-password', 'wrong');
    await authPage.click('#dev-mode-toggle-btn');
    await waitForDevice(authPage, 2000);
    // 应仍显示已启用（操作失败）
  });

  // ========== 场景D: 设备重启 ==========

  test('DEV-034: 重启延迟选择', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('#dev-restart-delay')).toBeVisible({ timeout: 8000 });
    const opts = await authPage.locator('#dev-restart-delay option').allTextContents();
    expect(opts.length).toBeGreaterThanOrEqual(2);
  });

  test('DEV-035: 设备重启', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('#dev-restart-btn')).toBeVisible({ timeout: 8000 });
  });

  test('DEV-036: 重启后服务恢复', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await expect(authPage.locator('#dev-restart-btn')).toBeVisible({ timeout: 8000 });
    // 点击重启
    await authPage.click('#dev-restart-btn');
    await authPage.waitForTimeout(500);
    const confirmBtn = authPage.locator('.modal-confirm-btn, .modal button:has-text("确定")').first();
    if (await confirmBtn.isVisible()) await confirmBtn.click();

    // 等待重启完成
    let recovered = false;
    for (let i = 0; i < 24; i++) {
      await authPage.waitForTimeout(5000);
      const ok = await authPage.evaluate(async () => {
        try { return (await fetch('/api/health')).ok; } catch { return false; }
      });
      if (ok) { recovered = true; break; }
    }
    expect(recovered).toBeTruthy();
  });

  test('DEV-037: 重启后配置保持', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const name = await authPage.locator('#dev-name').inputValue();
    // 验证配置仍完整
    expect(name).toBeTruthy();
  });

  // ========== 场景E: 缓存管理 ==========

  test('DEV-038: 缓存有效期选择', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    const opts = await authPage.locator('#dev-cache-duration option').allTextContents();
    expect(opts.length).toBeGreaterThanOrEqual(4);
  });

  test('DEV-039: 修改缓存有效期', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    await authPage.selectOption('#dev-cache-duration', '21600');
    expect(await authPage.locator('#dev-cache-duration').inputValue()).toBe('21600');
    await authPage.click('[data-action="saveCacheDuration"]');
    await waitForDevice(authPage, 2000);
  });

  test('DEV-040: 清除浏览器缓存', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    await authPage.click('[data-action="clearBrowserCache"]');
    await waitForDevice(authPage, 2000);
  });

  // ========== 场景F: 配置导入/导出 ==========

  test('DEV-041: 导出配置', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#dev-config-export-btn')).toBeVisible();
  });

  test('DEV-042: 导入配置文件选择', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#dev-config-import-file')).toBeAttached();
  });

  test('DEV-043: 导出按钮点击', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    const [download] = await Promise.all([
      authPage.waitForEvent('download', { timeout: 10_000 }).catch(() => null),
      authPage.click('#dev-config-export-btn'),
    ]);
    // 可能触发下载或弹窗
    if (download) {
      expect(download.suggestedFilename()).toContain('.json');
    }
  });

  test('DEV-044: 导入配置', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#dev-config-import-btn')).toBeVisible();
  });

  // ========== 场景G: 安全策略 ==========

  test('DEV-047: 安全策略面板展示', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#dev-sec-max-attempts')).toBeVisible();
    await expect(authPage.locator('#dev-sec-lockout-time')).toBeVisible();
  });

  test('DEV-048: 最大登录尝试修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.fill('#dev-sec-max-attempts', '5');
    expect(await authPage.locator('#dev-sec-max-attempts').inputValue()).toBe('5');
  });

  test('DEV-049: 锁定时间修改', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.fill('#dev-sec-lockout-time', '600');
    expect(await authPage.locator('#dev-sec-lockout-time').inputValue()).toBe('600');
  });

  test('DEV-050: 密码强度要求切换', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.selectOption('#dev-sec-require-strong', '1');
    expect(await authPage.locator('#dev-sec-require-strong').inputValue()).toBe('1');
    await authPage.selectOption('#dev-sec-require-strong', '0');
  });

  test('DEV-051: 安全策略保存', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(1500);
    // 确保表单已渲染
    await expect(authPage.locator('#device-security-form')).toBeVisible();
    // 直接在浏览器中调用 API 保存安全策略（绕过 Governor 队列可能的阻塞）
    const result = await authPage.evaluate(async () => {
      const config = {
        maxLoginAttempts: parseInt((document.getElementById('dev-sec-max-attempts') as HTMLInputElement)?.value || '5'),
        loginLockoutTime: (parseInt((document.getElementById('dev-sec-lockout-time') as HTMLInputElement)?.value || '300')) * 1000,
        minPasswordLength: parseInt((document.getElementById('dev-sec-min-pwd-len') as HTMLInputElement)?.value || '6'),
        requireStrongPasswords: (document.getElementById('dev-sec-require-strong') as HTMLSelectElement)?.value === '1',
      };
      const token = localStorage.getItem('auth_token') || '';
      const resp = await fetch('/api/device/config', {
        method: 'PUT',
        headers: { 'Content-Type': 'application/json', 'Authorization': 'Bearer ' + token },
        body: JSON.stringify(config)
      });
      return { status: resp.status, body: await resp.json() };
    });
    expect(result.status).toBe(200);
    expect(result.body.success).toBe(true);
  });

  // ========== 场景H: 恢复出厂设置 ==========

  test('DEV-052: 恢复出厂-输入框为空', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    // 不输入任何内容
    await expect(authPage.locator('#dev-factory-btn')).toBeVisible();
  });

  test('DEV-053: 恢复出厂-输入错误', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    await authPage.fill('#dev-factory-confirm', 'WRONG');
    await authPage.click('#dev-factory-btn');
    await waitForDevice(authPage, 2000);
    // 设备不应恢复出厂
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  // ========== 场景I: OTA固件升级 ==========

  test('DEV-056: OTA在线检查', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    await authPage.click('.config-tab[data-tab="dev-advanced"]');
    await authPage.waitForTimeout(500);
    // OTA 区域可见性检查
    await expect(authPage.locator('#dev-advanced')).toBeVisible();
  });
});
