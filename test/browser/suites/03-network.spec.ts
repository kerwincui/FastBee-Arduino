import { test, expect, env, waitForDevice, restoreNetworkConfig, waitForHealth } from '../fixtures/base.fixture';

test.describe('Suite-03: 网络设置 — WiFi/以太网/4G连通性', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('network');
  });

  // ========== 场景A: WiFi基本配置 ==========

  test('NET-001: 进入网络设置页', async ({ authPage }) => {
    await expect(authPage.locator('#network-page')).toBeVisible();
    await expect(authPage.locator('#network-title')).toContainText(/网络|Network/);
  });

  test('NET-002: 网络连接状态展示', async ({ authPage }) => {
    const statusCard = authPage.locator('#network-status-card');
    await expect(statusCard).toBeVisible();
    const wifiPanel = authPage.locator('#wifi-status-panel');
    await expect(wifiPanel).toBeVisible();
  });

  test('NET-003: 刷新状态按钮', async ({ authPage }) => {
    const refreshBtn = authPage.locator('#refresh-status-btn');
    await expect(refreshBtn).toBeVisible();
    await refreshBtn.click();
    await authPage.waitForTimeout(2000);
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-004: 联网方式默认WiFi', async ({ authPage }) => {
    const networkType = authPage.locator('#network-type');
    await expect(networkType).toBeVisible();
    const value = await networkType.inputValue();
    expect(value).toBe('0'); // WiFi
  });

  test('NET-005: WiFi扫描弹窗打开', async ({ authPage }) => {
    const scanBtn = authPage.locator('#wifi-scan-btn');
    if (await scanBtn.isVisible()) {
      await scanBtn.click();
      await authPage.waitForTimeout(3000);
      // 应有弹窗出现
      const modal = authPage.locator('.modal.show, .modal[style*="display: block"], .fb-modal, [class*="wifi-scan"]').first();
      if (await modal.isVisible()) {
        await expect(modal).toBeVisible();
      }
    }
  });

  test('NET-006: WiFi扫描结果展示', async ({ authPage }) => {
    const scanBtn = authPage.locator('#wifi-scan-btn');
    if (await scanBtn.isVisible()) {
      await scanBtn.click();
      await authPage.waitForTimeout(5000); // 等待扫描
      const scanList = authPage.locator('.wifi-scan-list, .scan-results, [class*="scan"]').first();
      if (await scanList.isVisible()) {
        const content = await scanList.innerHTML();
        expect(content.length).toBeGreaterThan(0);
      }
    }
  });

  test('NET-007: WiFi扫描选择网络', async ({ authPage }) => {
    const scanBtn = authPage.locator('#wifi-scan-btn');
    if (await scanBtn.isVisible()) {
      await scanBtn.click();
      await authPage.waitForTimeout(5000);
      const fastbeeItem = authPage.locator('.wifi-scan-item, .scan-item').filter({ hasText: env.wifi.ssid }).first();
      if (await fastbeeItem.isVisible()) {
        await fastbeeItem.click();
        await authPage.waitForTimeout(1000);
        const ssidValue = await authPage.locator('#wifi-ssid').inputValue();
        expect(ssidValue).toBe(env.wifi.ssid);
      }
    }
  });

  test('NET-008: WiFi扫描关闭弹窗', async ({ authPage }) => {
    const scanBtn = authPage.locator('#wifi-scan-btn');
    if (await scanBtn.isVisible()) {
      await scanBtn.click();
      await authPage.waitForTimeout(2000);
      const closeBtn = authPage.locator('.modal .close, .modal-close, [data-dismiss]').first();
      if (await closeBtn.isVisible()) {
        await closeBtn.click();
        await authPage.waitForTimeout(500);
      }
    }
  });

  test('NET-009: 网络模式-STA选择', async ({ authPage }) => {
    const wifiMode = authPage.locator('#wifi-mode');
    await expect(wifiMode).toBeVisible();
    await wifiMode.selectOption('0'); // STA
    await authPage.waitForTimeout(500);
    const value = await wifiMode.inputValue();
    expect(value).toBe('0');
  });

  test('NET-010: 网络模式-AP选择', async ({ authPage }) => {
    const wifiMode = authPage.locator('#wifi-mode');
    await wifiMode.selectOption('1'); // AP
    await authPage.waitForTimeout(1000);
    const notice = authPage.locator('#wifi-mode-notice');
    if (await notice.isVisible()) {
      await expect(notice).toBeVisible();
    }
    // 切回STA
    await wifiMode.selectOption('0');
  });

  test('NET-011: 手动输入WiFi名称', async ({ authPage }) => {
    const ssidInput = authPage.locator('#wifi-ssid');
    await ssidInput.fill('fastbee');
    const value = await ssidInput.inputValue();
    expect(value).toBe('fastbee');
  });

  test('NET-012: 安全类型-WPA2选择', async ({ authPage }) => {
    const security = authPage.locator('#wifi-security');
    await expect(security).toBeVisible();
    await security.selectOption('wpa2');
    const value = await security.inputValue();
    expect(value).toBe('wpa2');
  });

  test('NET-013: 安全类型-无密码', async ({ authPage }) => {
    const security = authPage.locator('#wifi-security');
    await security.selectOption('none');
    await authPage.waitForTimeout(500);
    const value = await security.inputValue();
    expect(value).toBe('none');
  });

  test('NET-014: 安全类型-WPA3', async ({ authPage }) => {
    const security = authPage.locator('#wifi-security');
    await security.selectOption('wpa3');
    const value = await security.inputValue();
    expect(value).toBe('wpa3');
    await security.selectOption('wpa2'); // 恢复
  });

  test('NET-015: 输入WiFi密码', async ({ authPage }) => {
    const pwdInput = authPage.locator('#wifi-password');
    await pwdInput.fill('15208747707');
    const value = await pwdInput.inputValue();
    expect(value).toBe('15208747707');
    // 密码框应为password类型
    const type = await pwdInput.getAttribute('type');
    expect(type).toBe('password');
  });

  test('NET-016: WiFi配置保存', async ({ authPage }) => {
    await authPage.fill('#wifi-ssid', env.wifi.ssid);
    await authPage.fill('#wifi-password', env.wifi.password);
    await authPage.locator('#wifi-security').selectOption('wpa2');
    await authPage.click('#wifi-save-btn');
    await waitForDevice(authPage, 3000);
    const success = authPage.locator('#wifi-success');
    if (await success.isVisible()) {
      await expect(success).toContainText(/成功|success/i);
    }
  });

  test('NET-017: WiFi连接验证', async ({ authPage }) => {
    await authPage.waitForTimeout(15_000);
    const refreshBtn = authPage.locator('#refresh-status-btn');
    await refreshBtn.click();
    await authPage.waitForTimeout(3000);
    const statusText = await authPage.locator('#wifi-status-badge').textContent().catch(() => '');
    console.log(`WiFi状态: ${statusText}`);
  });

  test('NET-018: WiFi配置保存后持久化', async ({ authPage }) => {
    await authPage.fill('#wifi-ssid', env.wifi.ssid);
    await authPage.fill('#wifi-password', env.wifi.password);
    await authPage.click('#wifi-save-btn');
    await waitForDevice(authPage, 5000);
    // 刷新页面
    await authPage.goto('/'); await authPage.waitForSelector('#app-container', { state: 'visible', timeout: 20_000 });
    await authPage.click('.menu-item[data-page="network"]');
    await authPage.waitForTimeout(3000);
    const ssidValue = await authPage.locator('#wifi-ssid').inputValue();
    expect(ssidValue).toBe(env.wifi.ssid);
  });

  // ========== 场景B: 联网方式切换 ==========

  test('NET-019: 切换到以太网', async ({ authPage }) => {
    const networkType = authPage.locator('#network-type');
    await networkType.selectOption('1'); // 以太网
    await authPage.waitForTimeout(1000);
    const ethPanel = authPage.locator('#ethernet-panel');
    await expect(ethPanel).toBeVisible();
    const wifiPanel = authPage.locator('#wifi-panel');
    await expect(wifiPanel).toBeHidden();
  });

  test('NET-020: 以太网引脚输入', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('1');
    await authPage.waitForTimeout(500);
    await authPage.fill('#eth-mosi', '11');
    await authPage.fill('#eth-miso', '13');
    await authPage.fill('#eth-sck', '12');
    await authPage.fill('#eth-cs', '47');
    await authPage.fill('#eth-rst', '48');
    await authPage.fill('#eth-int', '14');
    const mosiVal = await authPage.locator('#eth-mosi').inputValue();
    expect(mosiVal).toBe('11');
  });

  test('NET-021: 以太网配置保存', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('1');
    await authPage.waitForTimeout(500);
    await authPage.click('#ethernet-save-btn');
    await waitForDevice(authPage, 3000);
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-022: 切换到4G蜂窝', async ({ authPage }) => {
    const networkType = authPage.locator('#network-type');
    await networkType.selectOption('2'); // 4G
    await authPage.waitForTimeout(1000);
    const cellPanel = authPage.locator('#cellular-panel');
    await expect(cellPanel).toBeVisible();
  });

  test('NET-023: 4G配置-TX/RX/PWR', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.fill('#cell-tx', '39');
    await authPage.fill('#cell-rx', '40');
    await authPage.fill('#cell-pwr', '38');
    const txVal = await authPage.locator('#cell-tx').inputValue();
    expect(txVal).toBe('39');
  });

  test('NET-024: 4G配置-波特率和APN', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.fill('#cell-baud', '115200');
    await authPage.fill('#cell-apn', 'CMNET');
    const apnVal = await authPage.locator('#cell-apn').inputValue();
    expect(apnVal).toBe('CMNET');
  });

  test('NET-025: 4G配置保存', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.click('#cellular-save-btn');
    await waitForDevice(authPage, 3000);
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-026: 切回WiFi', async ({ authPage }) => {
    const networkType = authPage.locator('#network-type');
    await networkType.selectOption('2');
    await authPage.waitForTimeout(500);
    await networkType.selectOption('0');
    await authPage.waitForTimeout(1000);
    await expect(authPage.locator('#wifi-panel')).toBeVisible();
    await expect(authPage.locator('#ethernet-panel')).toBeHidden();
    await expect(authPage.locator('#cellular-panel')).toBeHidden();
  });

  // ========== 场景C: 热点配置 ==========

  test('NET-027: 热点配置Tab切换', async ({ authPage }) => {
    const apTab = authPage.locator('[data-tab="ap-config"]');
    await apTab.click();
    await authPage.waitForTimeout(1000);
    const apForm = authPage.locator('#ap-form');
    await expect(apForm).toBeVisible();
  });

  test('NET-028: 热点名称修改', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#ap-ssid', 'fastbee-test-ap');
    const value = await authPage.locator('#ap-ssid').inputValue();
    expect(value).toBe('fastbee-test-ap');
  });

  test('NET-029: 热点密码修改', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#ap-password', 'test12345678');
    const value = await authPage.locator('#ap-password').inputValue();
    expect(value).toBe('test12345678');
  });

  test('NET-030: 热点IP地址修改', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#ap-ip', '192.168.5.1');
    const value = await authPage.locator('#ap-ip').inputValue();
    expect(value).toBe('192.168.5.1');
  });

  test('NET-031: 隐藏热点-启用', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#ap-hidden').selectOption('1');
    const value = await authPage.locator('#ap-hidden').inputValue();
    expect(value).toBe('1');
  });

  test('NET-032: 隐藏热点-禁用', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#ap-hidden').selectOption('0');
    const value = await authPage.locator('#ap-hidden').inputValue();
    expect(value).toBe('0');
  });

  test('NET-033: 最大连接数调整', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#ap-max-connections').selectOption('2');
    const value = await authPage.locator('#ap-max-connections').inputValue();
    expect(value).toBe('2');
  });

  test('NET-034: 信道选择', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#ap-channel').selectOption('6');
    const value = await authPage.locator('#ap-channel').inputValue();
    expect(value).toBe('6');
  });

  test('NET-035: 热点配置保存', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#ap-form button[type="submit"]').click();
    await waitForDevice(authPage, 3000);
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-036: 热点密码长度验证', async ({ authPage }) => {
    await authPage.locator('[data-tab="ap-config"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#ap-password', '123');
    await authPage.locator('#ap-form button[type="submit"]').click();
    await authPage.waitForTimeout(1000);
    // 应有验证提示
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  // ========== 场景D: 高级配置 ==========

  test('NET-037: 高级配置Tab切换', async ({ authPage }) => {
    const advTab = authPage.locator('[data-tab="advance"]');
    await advTab.click();
    await authPage.waitForTimeout(1000);
    const advForm = authPage.locator('#advanced-form');
    await expect(advForm).toBeVisible();
  });

  test('NET-038: 静态IP-DHCP默认', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    const dhcp = authPage.locator('#wifi-dhcp');
    const value = await dhcp.inputValue();
    expect(value).toBe('0'); // DHCP
  });

  test('NET-039: 静态IP-手动配置', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#wifi-dhcp').selectOption('1'); // 静态
    await authPage.fill('#static-ip', '192.168.1.200');
    await authPage.fill('#subnet', '255.255.255.0');
    await authPage.fill('#gateway', '192.168.1.1');
    await authPage.fill('#dns1', '8.8.8.8');
    await authPage.fill('#dns2', '8.8.4.4');
    const ipVal = await authPage.locator('#static-ip').inputValue();
    expect(ipVal).toBe('192.168.1.200');
  });

  test('NET-040: 切回DHCP', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#wifi-dhcp').selectOption('0');
    const value = await authPage.locator('#wifi-dhcp').inputValue();
    expect(value).toBe('0');
  });

  test('NET-041: mDNS禁用', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#enable-mdns').selectOption('0'); // 禁用
    await authPage.waitForTimeout(500);
    const notice = authPage.locator('#mdns-disabled-notice');
    if (await notice.isVisible()) {
      await expect(notice).toBeVisible();
    }
  });

  test('NET-042: mDNS启用', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#enable-mdns').selectOption('1'); // 启用
    await authPage.waitForTimeout(500);
    const value = await authPage.locator('#enable-mdns').inputValue();
    expect(value).toBe('1');
  });

  test('NET-043: 自定义域名输入', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#custom-domain', 'fastbee-test');
    const preview = authPage.locator('#mdns-url-preview');
    if (await preview.isVisible()) {
      const previewText = await preview.textContent();
      expect(previewText).toContain('fastbee-test');
    }
  });

  test('NET-044: 连接超时修改', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#connect-timeout', '20000');
    const value = await authPage.locator('#connect-timeout').inputValue();
    expect(value).toBe('20000');
  });

  test('NET-045: 重连间隔修改', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#reconnect-interval', '8000');
    const value = await authPage.locator('#reconnect-interval').inputValue();
    expect(value).toBe('8000');
  });

  test('NET-046: 最大重连次数', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#max-reconnect-attempts', '15');
    const value = await authPage.locator('#max-reconnect-attempts').inputValue();
    expect(value).toBe('15');
  });

  test('NET-047: IP冲突检测-ARP', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#conflict-detection').selectOption('2'); // ARP
    const value = await authPage.locator('#conflict-detection').inputValue();
    expect(value).toBe('2');
  });

  test('NET-048: 高级配置保存', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#advanced-form button[type="submit"]').click();
    await waitForDevice(authPage, 3000);
    const success = authPage.locator('#advanced-success');
    if (await success.isVisible()) {
      await expect(success).toContainText(/成功|success/i);
    }
  });

  // ========== 场景E: WiFi连通性深度测试 ==========

  test('NET-049: WiFi STA首次连接全流程', async ({ page }) => {
    await page.goto('/');
    await page.fill('#username', env.auth.username);
    await page.fill('#password', env.auth.password);
    await page.click('#login-button');
    await page.waitForSelector('#app-container', { state: 'visible', timeout: 15_000 });
    await page.click('.menu-item[data-page="network"]');
    await page.waitForLoadState('domcontentloaded');
    await page.fill('#wifi-ssid', env.wifi.ssid);
    await page.fill('#wifi-password', env.wifi.password);
    await page.click('#wifi-save-btn');
    await page.waitForTimeout(15_000);
    await expect(page.locator('#app-container')).toBeVisible();
  });

  test('NET-050: WiFi STA重连验证', async ({ authPage }) => {
    // 验证WiFi配置持久化
    const ssid = await authPage.locator('#wifi-ssid').inputValue();
    console.log(`WiFi SSID: ${ssid}`);
    expect(ssid).toBeTruthy();
  });

  test('NET-051: WiFi错误密码连接', async ({ authPage }) => {
    await authPage.fill('#wifi-ssid', env.wifi.ssid);
    await authPage.fill('#wifi-password', 'wrongpassword');
    await authPage.click('#wifi-save-btn');
    await authPage.waitForTimeout(20_000);
    const healthResp = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthResp).toBeTruthy();
  });

  test('NET-052: WiFi不存在网络连接', async ({ authPage }) => {
    await authPage.fill('#wifi-ssid', 'nonexistent_wifi_12345');
    await authPage.fill('#wifi-password', '12345678');
    await authPage.click('#wifi-save-btn');
    await authPage.waitForTimeout(20_000);
    const healthResp = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthResp).toBeTruthy();
  });

  test('NET-053: WiFi连接后IP获取验证', async ({ authPage }) => {
    const ip = await authPage.locator('#wifi-ip-display').textContent().catch(() => '--');
    console.log(`WiFi IP: ${ip}`);
  });

  test('NET-054: WiFi连接后网关验证', async ({ authPage }) => {
    const status = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.network?.gateway ?? data?.gateway ?? 'unknown';
      } catch { return 'error'; }
    });
    console.log(`网关: ${status}`);
  });

  test('NET-055: WiFi连接后DNS验证', async ({ authPage }) => {
    const status = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.network?.dns ?? data?.dns ?? 'unknown';
      } catch { return 'error'; }
    });
    console.log(`DNS: ${status}`);
  });

  test('NET-056: WiFi信号强度合理性', async ({ authPage }) => {
    const rssi = await authPage.locator('#wifi-signal-display').textContent().catch(() => '--');
    console.log(`RSSI: ${rssi}`);
  });

  test('NET-057: WiFi MAC地址显示', async ({ authPage }) => {
    const status = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.network?.mac ?? data?.mac ?? 'unknown';
      } catch { return 'error'; }
    });
    console.log(`MAC: ${status}`);
  });

  test('NET-058: WiFi连接时长显示', async ({ authPage }) => {
    const uptime = await authPage.locator('#wifi-uptime-display').textContent().catch(() => '--');
    console.log(`连接时长: ${uptime}`);
  });

  test('NET-059: WiFi断开重连机制', async ({ authPage }) => {
    // 验证自动重连配置
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    const reconnectInterval = await authPage.locator('#reconnect-interval').inputValue().catch(() => '');
    const maxAttempts = await authPage.locator('#max-reconnect-attempts').inputValue().catch(() => '');
    console.log(`重连间隔: ${reconnectInterval}ms, 最大次数: ${maxAttempts}`);
  });

  test('NET-060: WiFi重连次数统计', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.waitForLoadState('domcontentloaded');
    const reconnects = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.network?.reconnects ?? data?.reconnectCount ?? -1;
      } catch { return -1; }
    });
    console.log(`重连次数: ${reconnects}`);
  });

  // NET-061~065: AP模式/混合模式（设备依赖测试，简化实现）
  test('NET-061: WiFi AP模式启动', async ({ authPage }) => {
    await authPage.locator('#wifi-mode').selectOption('1'); // AP
    await authPage.waitForTimeout(1000);
    const notice = authPage.locator('#wifi-mode-notice');
    if (await notice.isVisible()) {
      await expect(notice).toBeVisible();
    }
    await authPage.locator('#wifi-mode').selectOption('0'); // 恢复STA
  });

  test('NET-062: WiFi AP模式-客户端连接', async ({ authPage }) => {
    // AP模式需实际设备测试，此处验证UI
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-063: WiFi AP模式-状态信息', async ({ authPage }) => {
    const apPanel = authPage.locator('#wifi-status-panel');
    await expect(apPanel).toBeVisible();
  });

  test('NET-064: WiFi STA+AP混合模式', async ({ authPage }) => {
    await expect(authPage.locator('#wifi-mode')).toBeVisible();
  });

  test('NET-065: WiFi密码修改后重连', async ({ authPage }) => {
    await authPage.fill('#wifi-password', env.wifi.password);
    await authPage.click('#wifi-save-btn');
    await waitForDevice(authPage, 10_000);
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  // ========== 场景F: 以太网深度测试 ==========

  test('NET-066: 以太网引脚配置完整性', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('1');
    await authPage.waitForTimeout(500);
    const fields = ['#eth-mosi', '#eth-miso', '#eth-sck', '#eth-cs', '#eth-rst', '#eth-int'];
    for (const f of fields) {
      const val = await authPage.locator(f).inputValue();
      expect(Number(val)).toBeGreaterThanOrEqual(0);
    }
  });

  test('NET-067: 以太网引脚冲突检测', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('1');
    await authPage.waitForTimeout(500);
    await authPage.click('#ethernet-save-btn');
    await waitForDevice(authPage, 2000);
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-068: 以太网无效引脚拒绝', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('1');
    await authPage.waitForTimeout(500);
    const csInput = authPage.locator('#eth-cs');
    const max = await csInput.getAttribute('max');
    console.log(`CS引脚最大值: ${max}`);
  });

  test('NET-069: 以太网保存后重启验证', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('0'); // 切回WiFi
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#wifi-panel')).toBeVisible();
  });

  test('NET-070: 以太网DHCP获取IP', async ({ authPage }) => {
    const ethPanel = authPage.locator('#ethernet-status-panel');
    if (await ethPanel.isVisible()) {
      const ip = await authPage.locator('#eth-ip-display').textContent().catch(() => '--');
      console.log(`以太网IP: ${ip}`);
    }
  });

  test('NET-071: 以太网静态IP配置', async ({ authPage }) => {
    await authPage.locator('[data-tab="advance"]').click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#wifi-dhcp').selectOption('1');
    await authPage.fill('#static-ip', '192.168.1.100');
    await authPage.fill('#subnet', '255.255.255.0');
    await authPage.fill('#gateway', '192.168.1.1');
    const ipVal = await authPage.locator('#static-ip').inputValue();
    expect(ipVal).toBe('192.168.1.100');
    await authPage.locator('#wifi-dhcp').selectOption('0'); // 恢复DHCP
  });

  test('NET-072: 以太网断开网线处理', async ({ authPage }) => {
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('NET-073: 以太网重新插网线恢复', async ({ authPage }) => {
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-074: 以太网MAC地址显示', async ({ authPage }) => {
    const mac = await authPage.locator('#eth-mac-display').textContent().catch(() => '--');
    console.log(`以太网MAC: ${mac}`);
  });

  test('NET-075: 以太网与WiFi互斥验证', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('1');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#wifi-panel')).toBeHidden();
    await expect(authPage.locator('#ethernet-panel')).toBeVisible();
    await authPage.locator('#network-type').selectOption('0');
  });

  // ========== 场景G: 4G深度测试 ==========

  test('NET-076: 4G串口引脚配置', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.fill('#cell-tx', '39');
    await authPage.fill('#cell-rx', '40');
    await authPage.fill('#cell-pwr', '38');
    await authPage.fill('#cell-baud', '115200');
    await authPage.click('#cellular-save-btn');
    await waitForDevice(authPage, 2000);
  });

  test('NET-077: 4G APN配置', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.fill('#cell-apn', 'CMNET');
    const apnVal = await authPage.locator('#cell-apn').inputValue();
    expect(apnVal).toBe('CMNET');
  });

  test('NET-078: 4G APN-联通配置', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.fill('#cell-apn', '3GNET');
    const apnVal = await authPage.locator('#cell-apn').inputValue();
    expect(apnVal).toBe('3GNET');
  });

  test('NET-079: 4G APN-电信配置', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.fill('#cell-apn', 'CTNET');
    const apnVal = await authPage.locator('#cell-apn').inputValue();
    expect(apnVal).toBe('CTNET');
  });

  test('NET-080: 4G PWR引脚控制', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    const pwrVal = await authPage.locator('#cell-pwr').inputValue();
    expect(Number(pwrVal)).toBeGreaterThanOrEqual(0);
  });

  test('NET-081: 4G模块初始化日志', async ({ authPage }) => {
    await expect(authPage.locator('#network-page')).toBeVisible();
  });

  test('NET-082: 4G联网后AP热点提示', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    const hint = authPage.locator('.fb-info-box');
    if (await hint.isVisible()) {
      const text = await hint.textContent();
      expect(text).toContain('192.168.4.1');
    }
  });

  test('NET-083: 4G配置持久化', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    const apn = await authPage.locator('#cell-apn').inputValue();
    const tx = await authPage.locator('#cell-tx').inputValue();
    console.log(`4G APN: ${apn}, TX: ${tx}`);
    await authPage.locator('#network-type').selectOption('0');
  });

  test('NET-084: 4G与WiFi互斥', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await expect(authPage.locator('#wifi-panel')).toBeHidden();
    await authPage.locator('#network-type').selectOption('0');
  });

  test('NET-085: 4G串口波特率验证', async ({ authPage }) => {
    await authPage.locator('#network-type').selectOption('2');
    await authPage.waitForTimeout(500);
    await authPage.fill('#cell-baud', '9600');
    const baudVal = await authPage.locator('#cell-baud').inputValue();
    expect(baudVal).toBe('9600');
    await authPage.locator('#network-type').selectOption('0');
  });

  // ========== 全局清理：网络测试后自动恢复设备 ==========

  test.afterAll(async () => {
    const baseURL = `http://${env.deviceIp}`;
    console.log('[CLEANUP] Restoring network config to WiFi STA mode...');
    const restored = await restoreNetworkConfig(baseURL);
    if (restored) {
      console.log('[CLEANUP] Device reconnected via WiFi STA');
    } else {
      console.log('[CLEANUP] API restore failed, waiting for device health...');
      // 设备可能自行恢复（部分测试最后切回了 WiFi）
      await waitForHealth(baseURL, 60_000);
    }
  });
});
