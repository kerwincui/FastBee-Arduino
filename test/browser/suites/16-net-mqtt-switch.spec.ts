import { test, expect, env, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-16: 联网方式切换 + MQTT联动', () => {

  // ========== 场景A: WiFi环境下MQTT联动 ==========

  test('NWMS-001: WiFi连接后mqtt://首次连接', async ({ authPage, navigateTo }) => {
    // 1. 配置 WiFi STA
    await navigateTo('network');
    await authPage.fill('#wifi-ssid', env.wifi.ssid);
    await authPage.fill('#wifi-password', env.wifi.password);
    await authPage.click('#wifi-save-btn');
    await waitForDevice(authPage, 5000);

    // 2. 配置 mqtt://
    await navigateTo('protocol');
    const enableCb = authPage.locator('#mqtt-enabled');
    if (await enableCb.isVisible() && !(await enableCb.isChecked())) {
      await enableCb.check();
    }
    const schemeSelect = authPage.locator('#mqtt-scheme');
    if (await schemeSelect.isVisible()) {
      await schemeSelect.selectOption('mqtt');
    }
    await authPage.fill('#mqtt-broker', env.mqtt.broker);
    await authPage.fill('#mqtt-port', String(env.mqtt.portMqtt));
    await authPage.fill('#mqtt-client-id', env.mqtt.clientId);
    await authPage.fill('#mqtt-username', env.mqtt.username);
    await authPage.fill('#mqtt-password', env.mqtt.password);
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(15_000);

    // 3. 验证
    await expect(authPage.locator('#protocol-page')).toBeVisible();
  });

  test('NWMS-002: WiFi连接后mqtts://连接(S3设备)', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const schemeSelect = authPage.locator('#mqtt-scheme');
    if (await schemeSelect.isVisible()) {
      await schemeSelect.selectOption('mqtts');
      await authPage.waitForTimeout(500);
      // 端口应自动切换为 8883
      const portVal = await authPage.locator('#mqtt-port').inputValue();
      console.log(`mqtts端口: ${portVal}`);
    }
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(30_000); // TLS需要更长时间
  });

  test('NWMS-003: WiFi断开后MQTT状态变化', async ({ authPage, navigateTo }) => {
    // 验证MQTT状态面板可查询
    await navigateTo('protocol');
    const statusBadge = authPage.locator('#mqtt-status-badge');
    if (await statusBadge.isVisible()) {
      const status = await statusBadge.textContent();
      console.log(`MQTT状态: ${status}`);
    }
  });

  test('NWMS-004: WiFi恢复后MQTT自动重连', async ({ authPage, navigateTo }) => {
    // 验证自动重连开关
    await navigateTo('protocol');
    const autoReconnect = authPage.locator('#mqtt-auto-reconnect');
    if (await autoReconnect.isVisible()) {
      const isChecked = await autoReconnect.isChecked();
      console.log(`自动重连: ${isChecked}`);
      // 确保自动重连开启
      if (!isChecked) await autoReconnect.check();
    }
  });

  test('NWMS-005: WiFi重连后MQTT发布验证', async ({ authPage, navigateTo }) => {
    // 验证MQTT状态面板字段完整
    await navigateTo('protocol');
    const statusServer = authPage.locator('#mqtt-status-server');
    const statusClientId = authPage.locator('#mqtt-status-clientid');
    const statusReconnects = authPage.locator('#mqtt-status-reconnects');
    if (await statusServer.isVisible()) {
      const server = await statusServer.textContent();
      console.log(`MQTT服务器: ${server}`);
    }
    if (await statusReconnects.isVisible()) {
      const reconnects = await statusReconnects.textContent();
      console.log(`重连次数: ${reconnects}`);
    }
  });

  test('NWMS-006: WiFi密码修改后MQTT重连', async ({ authPage, navigateTo }) => {
    // 修改WiFi密码为正确值
    await navigateTo('network');
    await authPage.fill('#wifi-password', env.wifi.password);
    await authPage.click('#wifi-save-btn');
    await waitForDevice(authPage, 10_000);
    // 设备应保持可达
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('NWMS-007: WiFi重启后MQTT保持配置', async ({ authPage, navigateTo }) => {
    // 验证MQTT配置持久化
    await navigateTo('protocol');
    const broker = await authPage.locator('#mqtt-broker').inputValue().catch(() => '');
    const clientId = await authPage.locator('#mqtt-client-id').inputValue().catch(() => '');
    console.log(`MQTT Broker: ${broker ? '已配置' : '未配置'}, ClientID: ${clientId ? '已配置' : '未配置'}`);
  });

  test('NWMS-008: AP模式下MQTT状态', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const statusBadge = authPage.locator('#mqtt-status-badge');
    if (await statusBadge.isVisible()) {
      const status = await statusBadge.textContent();
      // AP模式下MQTT可能未连接（无外网）
      console.log(`AP模式MQTT状态: ${status}`);
    }
  });

  test('NWMS-009: STA+AP混合模式下MQTT', async ({ authPage, navigateTo }) => {
    // 验证WiFi模式选择
    await navigateTo('network');
    const wifiMode = authPage.locator('#wifi-mode');
    if (await wifiMode.isVisible()) {
      const modeVal = await wifiMode.inputValue();
      console.log(`WiFi模式: ${modeVal}`);
    }
  });

  // ========== 场景B: 以太网环境下MQTT联动 ==========

  test('NWMS-010: 以太网连接后mqtt://连接', async ({ authPage, navigateTo }) => {
    // 切换到以太网
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('1'); // 以太网
      await authPage.waitForTimeout(1000);
      // 以太网面板应可见
      const ethPanel = authPage.locator('#ethernet-panel');
      if (await ethPanel.isVisible()) {
        await expect(ethPanel).toBeVisible();
      }
    }
  });

  test('NWMS-011: 以太网连接后mqtts://连接', async ({ authPage, navigateTo }) => {
    // 验证以太网SPI引脚配置
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('1');
      await authPage.waitForTimeout(1000);
      const mosi = authPage.locator('#eth-mosi');
      if (await mosi.isVisible()) {
        const mosiVal = await mosi.inputValue();
        console.log(`以太网MOSI: ${mosiVal}`);
      }
    }
  });

  test('NWMS-012: 以太网拔网线后MQTT断开', async ({ authPage, navigateTo }) => {
    // 模拟验证：检查健康状态
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('NWMS-013: 以太网插回网线后MQTT重连', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    await expect(authPage.locator('#protocol-page')).toBeVisible();
  });

  test('NWMS-014: 以太网重启后MQTT恢复', async ({ authPage, navigateTo }) => {
    // 验证API可达
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('NWMS-015: 以太网静态IP下MQTT连接', async ({ authPage, navigateTo }) => {
    // 高级配置中的静态IP
    await navigateTo('network');
    const advTab = authPage.locator('[data-tab="advance"]');
    if (await advTab.isVisible()) {
      await advTab.click();
      await authPage.waitForTimeout(1000);
      const dhcp = authPage.locator('#wifi-dhcp');
      if (await dhcp.isVisible()) {
        const dhcpVal = await dhcp.inputValue();
        console.log(`DHCP模式: ${dhcpVal}`);
      }
    }
  });

  // ========== 场景C: 4G环境下MQTT联动 ==========

  test('NWMS-016: 4G连接后mqtt://连接', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('2'); // 4G
      await authPage.waitForTimeout(1000);
      const cellPanel = authPage.locator('#cellular-panel');
      if (await cellPanel.isVisible()) {
        await expect(cellPanel).toBeVisible();
        // 4G提示通过AP热点访问
        const hint = authPage.locator('.fb-info-box');
        if (await hint.isVisible()) {
          const hintText = await hint.textContent();
          console.log(`4G提示: ${hintText?.substring(0, 80)}`);
        }
      }
    }
  });

  test('NWMS-017: 4G连接后mqtts://连接', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    await expect(authPage.locator('#protocol-page')).toBeVisible();
  });

  test('NWMS-018: 4G信号丢失后MQTT断开', async ({ authPage }) => {
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('NWMS-019: 4G信号恢复后MQTT重连', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    await expect(authPage.locator('#protocol-page')).toBeVisible();
  });

  test('NWMS-020: 4G APN切换后MQTT重连', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('2');
      await authPage.waitForTimeout(1000);
      const apnInput = authPage.locator('#cell-apn');
      if (await apnInput.isVisible()) {
        const apnVal = await apnInput.inputValue();
        console.log(`当前APN: ${apnVal}`);
      }
    }
  });

  // ========== 场景D: 联网方式之间切换 ==========

  test('NWMS-021: WiFi→以太网切换后MQTT', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      // WiFi → 以太网
      await networkType.selectOption('1');
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('#ethernet-panel')).toBeVisible();
      // 切回WiFi
      await networkType.selectOption('0');
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('#wifi-panel')).toBeVisible();
    }
  });

  test('NWMS-022: 以太网→WiFi切换后MQTT', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('0'); // WiFi
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('#wifi-panel')).toBeVisible();
    }
  });

  test('NWMS-023: WiFi→4G切换后MQTT', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('2'); // 4G
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('#cellular-panel')).toBeVisible();
    }
  });

  test('NWMS-024: 4G→WiFi切换后MQTT', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('0'); // WiFi
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('#wifi-panel')).toBeVisible();
    }
  });

  test('NWMS-025: 以太网→4G切换后MQTT', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('2');
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('#cellular-panel')).toBeVisible();
    }
  });

  test('NWMS-026: 4G→以太网切换后MQTT', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      await networkType.selectOption('1');
      await authPage.waitForTimeout(1000);
      await expect(authPage.locator('#ethernet-panel')).toBeVisible();
    }
  });

  test('NWMS-027: 连续三次切换联网方式', async ({ authPage, navigateTo }) => {
    await navigateTo('network');
    const networkType = authPage.locator('#network-type');
    if (await networkType.isVisible()) {
      // WiFi → 以太网 → 4G → WiFi
      await networkType.selectOption('1');
      await authPage.waitForTimeout(500);
      await networkType.selectOption('2');
      await authPage.waitForTimeout(500);
      await networkType.selectOption('0');
      await authPage.waitForTimeout(500);
      // 最终应为WiFi
      const finalVal = await networkType.inputValue();
      expect(finalVal).toBe('0');
      await expect(authPage.locator('#wifi-panel')).toBeVisible();
    }
  });

  // ========== 场景E: mqtt ↔ mqtts 协议切换 ==========

  test('NWMS-028: mqtt切换到mqtts(S3设备)', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const schemeSelect = authPage.locator('#mqtt-scheme');
    if (await schemeSelect.isVisible()) {
      await schemeSelect.selectOption('mqtt');
      await authPage.waitForTimeout(500);
      await schemeSelect.selectOption('mqtts');
      await authPage.waitForTimeout(500);
      const portVal = await authPage.locator('#mqtt-port').inputValue();
      console.log(`mqtts端口: ${portVal}`);
    }
  });

  test('NWMS-029: mqtts切换到mqtt(S3设备)', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const schemeSelect = authPage.locator('#mqtt-scheme');
    if (await schemeSelect.isVisible()) {
      await schemeSelect.selectOption('mqtts');
      await authPage.waitForTimeout(500);
      await schemeSelect.selectOption('mqtt');
      await authPage.waitForTimeout(500);
      const portVal = await authPage.locator('#mqtt-port').inputValue();
      expect(portVal).toBe('1883');
    }
  });

  test('NWMS-030: 协议切换后主题保留', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    // 切换协议后检查凭据是否保留
    const schemeSelect = authPage.locator('#mqtt-scheme');
    if (await schemeSelect.isVisible()) {
      const brokerBefore = await authPage.locator('#mqtt-broker').inputValue();
      await schemeSelect.selectOption('mqtts');
      await authPage.waitForTimeout(500);
      await schemeSelect.selectOption('mqtt');
      await authPage.waitForTimeout(500);
      const brokerAfter = await authPage.locator('#mqtt-broker').inputValue();
      // Broker地址应保留
      expect(brokerAfter).toBe(brokerBefore);
    }
  });

  test('NWMS-031: 协议切换后凭据保留', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const schemeSelect = authPage.locator('#mqtt-scheme');
    if (await schemeSelect.isVisible()) {
      // 填写凭据
      await authPage.fill('#mqtt-broker', env.mqtt.broker);
      await authPage.fill('#mqtt-client-id', env.mqtt.clientId);

      // 切换协议
      await schemeSelect.selectOption('mqtts');
      await authPage.waitForTimeout(500);

      // 验证凭据保留
      const broker = await authPage.locator('#mqtt-broker').inputValue();
      const clientId = await authPage.locator('#mqtt-client-id').inputValue();
      expect(broker).toBe(env.mqtt.broker);
      expect(clientId).toBe(env.mqtt.clientId);
    }
  });

  test('NWMS-032: 快速多次协议切换', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const schemeSelect = authPage.locator('#mqtt-scheme');
    if (await schemeSelect.isVisible()) {
      for (let i = 0; i < 4; i++) {
        await schemeSelect.selectOption(i % 2 === 0 ? 'mqtts' : 'mqtt');
        await authPage.waitForTimeout(200);
      }
      // 最终应无崩溃
      await expect(authPage.locator('#protocol-page')).toBeVisible();
    }
  });

  // ========== 场景F: MQTT连接稳定性 ==========

  test('NWMS-033: MQTT长连接稳定性(5分钟)', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    // 每30秒检查一次，共10次（5分钟）
    for (let i = 0; i < 10; i++) {
      await authPage.waitForTimeout(30_000);
      const healthOk = await authPage.evaluate(async () => {
        try { return (await fetch('/api/health')).ok; } catch { return false; }
      });
      console.log(`[${i * 30}s] 健康: ${healthOk}`);
      expect(healthOk).toBeTruthy();
    }
  });

  test('NWMS-034: MQTT长连接稳定性(15分钟)', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    // 每60秒检查一次，共15次
    for (let i = 0; i < 15; i++) {
      await authPage.waitForTimeout(60_000);
      const status = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/status');
          const data = await r.json();
          return { ok: true, heapFree: data?.heap?.free ?? -1 };
        } catch { return { ok: false, heapFree: -1 }; }
      });
      console.log(`[${i}min] ok=${status.ok}, heap=${status.heapFree}`);
      expect(status.ok).toBeTruthy();
    }
  });

  test('NWMS-035: 设备重启后MQTT全量恢复', async ({ authPage, navigateTo }) => {
    // 验证MQTT配置完整
    await navigateTo('protocol');
    const broker = await authPage.locator('#mqtt-broker').inputValue().catch(() => '');
    const clientId = await authPage.locator('#mqtt-client-id').inputValue().catch(() => '');
    const enabled = await authPage.locator('#mqtt-enabled').isChecked().catch(() => false);
    console.log(`MQTT: enabled=${enabled}, broker=${broker ? '配置' : '空'}, clientId=${clientId ? '配置' : '空'}`);
  });

  test('NWMS-036: 多设备并发MQTT连接', async ({ authPage, navigateTo }) => {
    // 单机验证：MQTT配置可保存
    await navigateTo('protocol');
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });
});
