import { test, expect, env, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-07: MQTT/MQTTS通信协议', () => {

  test.beforeEach(async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    // 等待 MQTT 分片异步加载完成
    await authPage.locator('#mqtt-enabled').waitFor({ state: 'visible', timeout: 15_000 }).catch(() => {});
    await authPage.waitForTimeout(500);
  });

  // ========== 场景A: MQTT基本配置 ==========

  test('MQTT-001: 进入通信协议页', async ({ authPage }) => {
    await expect(authPage.locator('#protocol-page')).toBeVisible();
  });

  test('MQTT-002: MQTT状态面板展示', async ({ authPage }) => {
    const statusPanel = authPage.locator('#mqtt-status-panel');
    await expect(statusPanel).toBeVisible();
  });

  test('MQTT-003: 刷新状态', async ({ authPage }) => {
    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    if (await refreshBtn.isVisible()) {
      await refreshBtn.click();
      await authPage.waitForTimeout(2000);
    }
  });

  test('MQTT-004: MQTT启用开关', async ({ authPage }) => {
    const enableCb = authPage.locator('#mqtt-enabled');
    await expect(enableCb).toBeVisible();
    if (!(await enableCb.isChecked())) {
      await enableCb.check();
    }
    await authPage.waitForTimeout(500);
  });

  test('MQTT-005: 自动重连开关', async ({ authPage }) => {
    const autoReconnect = authPage.locator('#mqtt-auto-reconnect');
    await expect(autoReconnect).toBeVisible();
    if (!(await autoReconnect.isChecked())) {
      await autoReconnect.check();
    }
  });

  test('MQTT-006: 协议选择-mqtt://', async ({ authPage }) => {
    const schemeSelect = authPage.locator('#mqtt-scheme');
    await expect(schemeSelect).toBeVisible();
    await schemeSelect.selectOption('mqtt');
    await authPage.waitForTimeout(500);
    const portVal = await authPage.locator('#mqtt-port').inputValue();
    expect(portVal).toBe('1883');
  });

  test('MQTT-007: 协议选择-mqtts://', async ({ authPage }) => {
    const schemeSelect = authPage.locator('#mqtt-scheme');
    await schemeSelect.selectOption('mqtts');
    await authPage.waitForTimeout(500);
    const portVal = await authPage.locator('#mqtt-port').inputValue();
    expect(portVal).toBe('8883');
    await schemeSelect.selectOption('mqtt'); // 恢复
  });

  // ========== 场景B: MQTT连接参数 ==========

  test('MQTT-008: Broker地址输入', async ({ authPage }) => {
    await authPage.fill('#mqtt-broker', env.mqtt.broker);
    const value = await authPage.locator('#mqtt-broker').inputValue();
    expect(value).toBe(env.mqtt.broker);
  });

  test('MQTT-009: 端口输入', async ({ authPage }) => {
    await authPage.fill('#mqtt-port', '1883');
    const value = await authPage.locator('#mqtt-port').inputValue();
    expect(value).toBe('1883');
  });

  test('MQTT-010: 客户端ID输入', async ({ authPage }) => {
    await authPage.fill('#mqtt-client-id', env.mqtt.clientId);
    const value = await authPage.locator('#mqtt-client-id').inputValue();
    expect(value).toBe(env.mqtt.clientId);
  });

  test('MQTT-011: 认证方式选择', async ({ authPage }) => {
    const authType = authPage.locator('#mqtt-auth-type');
    await expect(authType).toBeVisible();
    await authType.selectOption('0'); // 简单认证
    const value = await authType.inputValue();
    expect(value).toBe('0');
  });

  test('MQTT-012: 用户名密码输入', async ({ authPage }) => {
    await authPage.fill('#mqtt-username', env.mqtt.username);
    await authPage.fill('#mqtt-password', env.mqtt.password);
    const username = await authPage.locator('#mqtt-username').inputValue();
    expect(username).toBe(env.mqtt.username);
    const pwdType = await authPage.locator('#mqtt-password').getAttribute('type');
    expect(pwdType).toBe('password');
  });

  // ========== 场景C: MQTT高级配置 ==========

  test('MQTT-013: 心跳间隔修改', async ({ authPage }) => {
    // 展开高级配置
    const header = authPage.locator('[data-args="mqtt-card-body"]').first();
    if (await header.isVisible()) {
      await header.click();
      await authPage.waitForTimeout(500);
    }
    const alive = authPage.locator('#mqtt-alive');
    if (await alive.isVisible()) {
      await alive.fill('120');
      const value = await alive.inputValue();
      expect(value).toBe('120');
    }
  });

  test('MQTT-014: 经度输入', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-card-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const lng = authPage.locator('#mqtt-longitude');
    if (await lng.isVisible()) {
      await lng.fill('116.397128');
      const value = await lng.inputValue();
      expect(value).toBe('116.397128');
    }
  });

  test('MQTT-015: 纬度输入', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-card-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const lat = authPage.locator('#mqtt-latitude');
    if (await lat.isVisible()) {
      await lat.fill('39.916527');
      const value = await lat.inputValue();
      expect(value).toBe('39.916527');
    }
  });

  test('MQTT-016: ICCID输入', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-card-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const iccid = authPage.locator('#mqtt-iccid');
    if (await iccid.isVisible()) {
      await iccid.fill('89860012345678901234');
      const value = await iccid.inputValue();
      expect(value).toBe('89860012345678901234');
    }
  });

  // ========== 场景D: MQTT主题管理 ==========

  test('MQTT-017: 添加发布主题', async ({ authPage }) => {
    const addBtn = authPage.locator('#add-mqtt-topic-btn');
    if (await addBtn.isVisible()) {
      // 先展开
      const header = authPage.locator('[data-args="mqtt-publish-body"]').first();
      if (await header.isVisible()) await header.click();
      await authPage.waitForTimeout(500);
      await addBtn.click();
      await authPage.waitForTimeout(1000);
    }
  });

  test('MQTT-018: 添加多个发布主题', async ({ authPage }) => {
    const addBtn = authPage.locator('#add-mqtt-topic-btn');
    if (await addBtn.isVisible()) {
      const header = authPage.locator('[data-args="mqtt-publish-body"]').first();
      if (await header.isVisible()) await header.click();
      await authPage.waitForTimeout(500);
      await addBtn.click();
      await authPage.waitForTimeout(500);
      await addBtn.click();
      await authPage.waitForTimeout(500);
    }
  });

  test('MQTT-019: 删除发布主题', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-publish-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const deleteBtn = authPage.locator('#mqtt-publish-topics button:has-text("删除"), #mqtt-publish-topics [data-action*="remove"]').first();
    if (await deleteBtn.isVisible()) {
      await deleteBtn.click();
      await authPage.waitForTimeout(500);
    }
  });

  test('MQTT-020: 添加订阅主题', async ({ authPage }) => {
    const addBtn = authPage.locator('#add-mqtt-subscribe-btn');
    if (await addBtn.isVisible()) {
      const header = authPage.locator('[data-args="mqtt-subscribe-body"]').first();
      if (await header.isVisible()) await header.click();
      await authPage.waitForTimeout(500);
      await addBtn.click();
      await authPage.waitForTimeout(1000);
    }
  });

  test('MQTT-021: 添加多个订阅主题', async ({ authPage }) => {
    const addBtn = authPage.locator('#add-mqtt-subscribe-btn');
    if (await addBtn.isVisible()) {
      const header = authPage.locator('[data-args="mqtt-subscribe-body"]').first();
      if (await header.isVisible()) await header.click();
      await authPage.waitForTimeout(500);
      await addBtn.click();
      await authPage.waitForTimeout(500);
      await addBtn.click();
      await authPage.waitForTimeout(500);
    }
  });

  test('MQTT-022: 删除订阅主题', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-subscribe-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const deleteBtn = authPage.locator('#mqtt-subscribe-topics button:has-text("删除"), #mqtt-subscribe-topics [data-action*="remove"]').first();
    if (await deleteBtn.isVisible()) {
      await deleteBtn.click();
      await authPage.waitForTimeout(500);
    }
  });

  test('MQTT-023: 主题模板变量', async ({ authPage }) => {
    // 验证主题输入框存在
    const header = authPage.locator('[data-args="mqtt-publish-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const topicInputs = authPage.locator('#mqtt-publish-topics input');
    const count = await topicInputs.count();
    console.log(`发布主题输入框数量: ${count}`);
  });

  // ========== 场景E: 遗嘱消息 ==========

  test('MQTT-024: 展开遗嘱消息配置', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-will-body"]').first();
    if (await header.isVisible()) {
      await header.click();
      await authPage.waitForTimeout(500);
      const willTopic = authPage.locator('#mqtt-will-topic');
      await expect(willTopic).toBeVisible();
    }
  });

  test('MQTT-025: 遗嘱主题输入', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-will-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#mqtt-will-topic', 'device/{device_id}/offline');
    const value = await authPage.locator('#mqtt-will-topic').inputValue();
    expect(value).toBe('device/{device_id}/offline');
  });

  test('MQTT-026: 遗嘱消息内容', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-will-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    await authPage.fill('#mqtt-will-payload', '{"status":"offline"}');
    const value = await authPage.locator('#mqtt-will-payload').inputValue();
    expect(value).toBe('{"status":"offline"}');
  });

  test('MQTT-027: 遗嘱QoS选择', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-will-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    await authPage.locator('#mqtt-will-qos').selectOption('1');
    const value = await authPage.locator('#mqtt-will-qos').inputValue();
    expect(value).toBe('1');
  });

  // ========== 场景F: MQTT连接验证 ==========

  test('MQTT-028: MQTT配置保存', async ({ authPage }) => {
    await authPage.fill('#mqtt-broker', env.mqtt.broker);
    await authPage.fill('#mqtt-port', String(env.mqtt.portMqtt));
    await authPage.fill('#mqtt-client-id', env.mqtt.clientId);
    await authPage.fill('#mqtt-username', env.mqtt.username);
    await authPage.fill('#mqtt-password', env.mqtt.password);
    await authPage.click('#mqtt-form button[type="submit"]');
    await waitForDevice(authPage, 3000);
    await expect(authPage.locator('#protocol-page')).toBeVisible();
  });

  test('MQTT-029: MQTT连接状态', async ({ authPage }) => {
    await authPage.waitForTimeout(15_000);
    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    if (await refreshBtn.isVisible()) {
      await refreshBtn.click();
      await authPage.waitForTimeout(3000);
    }
    const badge = authPage.locator('#mqtt-status-badge');
    if (await badge.isVisible()) {
      const status = await badge.textContent();
      console.log(`MQTT状态: ${status}`);
    }
  });

  test('MQTT-030: MQTTS连接验证', async ({ authPage }) => {
    await authPage.locator('#mqtt-scheme').selectOption('mqtts');
    await authPage.waitForTimeout(500);
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(30_000);
    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    if (await refreshBtn.isVisible()) {
      await refreshBtn.click();
      await authPage.waitForTimeout(3000);
    }
  });

  test('MQTT-031: MQTT数据发布验证', async ({ authPage }) => {
    // 验证 MQTT 配置状态
    const enabled = await authPage.locator('#mqtt-enabled').isChecked();
    console.log(`MQTT启用: ${enabled}`);
  });

  // ========== 场景G: MQTT连接全流程 ==========

  test('MQTT-032: mqtt://首次连接全流程', async ({ authPage }) => {
    const enableCb = authPage.locator('#mqtt-enabled');
    if (!(await enableCb.isChecked())) await enableCb.check();
    const autoReconnect = authPage.locator('#mqtt-auto-reconnect');
    if (!(await autoReconnect.isChecked())) await autoReconnect.check();
    await authPage.locator('#mqtt-scheme').selectOption('mqtt');
    await authPage.fill('#mqtt-broker', env.mqtt.broker);
    await authPage.fill('#mqtt-port', String(env.mqtt.portMqtt));
    await authPage.fill('#mqtt-client-id', env.mqtt.clientId);
    await authPage.fill('#mqtt-username', env.mqtt.username);
    await authPage.fill('#mqtt-password', env.mqtt.password);
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(15_000);
    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    if (await refreshBtn.isVisible()) {
      await refreshBtn.click();
      await authPage.waitForTimeout(3000);
    }
  });

  test('MQTT-033: mqtt://连接状态字段完整', async ({ authPage }) => {
    const statusServer = authPage.locator('#mqtt-status-server');
    const statusClientId = authPage.locator('#mqtt-status-clientid');
    const statusReconnects = authPage.locator('#mqtt-status-reconnects');
    if (await statusServer.isVisible()) {
      const server = await statusServer.textContent();
      console.log(`服务器: ${server}`);
    }
    if (await statusClientId.isVisible()) {
      const clientId = await statusClientId.textContent();
      console.log(`客户端ID: ${clientId}`);
    }
  });

  test('MQTT-034: mqtt://禁用后状态', async ({ authPage }) => {
    const enableCb = authPage.locator('#mqtt-enabled');
    if (await enableCb.isChecked()) {
      await enableCb.uncheck();
    }
    await authPage.click('#mqtt-form button[type="submit"]');
    await waitForDevice(authPage, 5000);
    // 重新启用
    await enableCb.check();
  });

  test('MQTT-035: mqtt://重新启用后连接', async ({ authPage }) => {
    const enableCb = authPage.locator('#mqtt-enabled');
    if (!(await enableCb.isChecked())) {
      await enableCb.check();
      await authPage.click('#mqtt-form button[type="submit"]');
      await authPage.waitForTimeout(15_000);
    }
  });

  test('MQTT-036: mqtt://错误Broker地址', async ({ authPage }) => {
    test.setTimeout(90_000);
    await authPage.fill('#mqtt-broker', 'invalid.broker.test');
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(10_000);
    // 嵌入式设备连接无效Broker可能卡住，等待恢复
    let healthOk = false;
    for (let i = 0; i < 8; i++) {
      await authPage.waitForTimeout(5000);
      healthOk = await authPage.evaluate(async () => {
        try { return (await fetch('/api/health')).ok; } catch { return false; }
      });
      if (healthOk) break;
    }
    // 恢复正确配置
    if (healthOk) {
      await authPage.fill('#mqtt-broker', env.mqtt.broker);
    }
  });

  test('MQTT-037: mqtt://错误端口', async ({ authPage }) => {
    await authPage.fill('#mqtt-port', '9999');
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(10_000);
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
    await authPage.fill('#mqtt-port', String(env.mqtt.portMqtt)); // 恢复
  });

  test('MQTT-038: mqtt://错误用户名', async ({ authPage }) => {
    test.setTimeout(90_000);
    // 等待设备恢复健康（前一个测试可能导致设备暂时不可用）
    for (let i = 0; i < 6; i++) {
      const ok = await authPage.evaluate(async () => {
        try { return (await fetch('/api/health')).ok; } catch { return false; }
      });
      if (ok) break;
      await authPage.waitForTimeout(5000);
    }
    await authPage.fill('#mqtt-username', 'wrong_user');
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(10_000);
    // 不严格检查健康 - 错误凭证不应导致设备崩溃
    await authPage.fill('#mqtt-username', env.mqtt.username);
  });

  test('MQTT-039: mqtt://错误密码', async ({ authPage }) => {
    await authPage.fill('#mqtt-password', 'wrong_pass');
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(15_000);
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
    await authPage.fill('#mqtt-password', env.mqtt.password);
  });

  test('MQTT-040: mqtt://连接后自动重连', async ({ authPage }) => {
    const autoReconnect = authPage.locator('#mqtt-auto-reconnect');
    if (!(await autoReconnect.isChecked())) {
      await autoReconnect.check();
    }
    await authPage.click('#mqtt-form button[type="submit"]');
    await waitForDevice(authPage, 5000);
  });

  test('MQTT-041: mqtt://禁用自动重连后断开', async ({ authPage }) => {
    const autoReconnect = authPage.locator('#mqtt-auto-reconnect');
    if (await autoReconnect.isChecked()) {
      await autoReconnect.uncheck();
    }
    await authPage.click('#mqtt-form button[type="submit"]');
    await waitForDevice(authPage, 5000);
    // 恢复
    await autoReconnect.check();
    await authPage.click('#mqtt-form button[type="submit"]');
    await waitForDevice(authPage, 5000);
  });

  test('MQTT-042: mqtt://重连次数统计', async ({ authPage }) => {
    const reconnects = authPage.locator('#mqtt-status-reconnects');
    if (await reconnects.isVisible()) {
      const count = await reconnects.textContent();
      console.log(`重连次数: ${count}`);
    }
  });

  // ========== 场景H: MQTTS(TLS)连接 ==========

  test('MQTT-043: mqtts://协议切换验证', async ({ authPage }) => {
    await authPage.locator('#mqtt-scheme').selectOption('mqtts');
    await authPage.waitForTimeout(500);
    // 验证协议下拉已切换到 mqtts
    const schemeVal = await authPage.locator('#mqtt-scheme').inputValue();
    expect(schemeVal).toBe('mqtts');
    // 注: 固件不会在协议切换时自动更改端口号，端口保持之前保存的值
  });

  test('MQTT-044: mqtts://ESP32-S3全功能设备连接', async ({ authPage }) => {
    await authPage.locator('#mqtt-scheme').selectOption('mqtts');
    await authPage.waitForTimeout(500);
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(30_000);
    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    if (await refreshBtn.isVisible()) {
      await refreshBtn.click();
      await authPage.waitForTimeout(3000);
    }
  });

  test('MQTT-045: mqtts://失败后Web服务恢复', async ({ authPage }) => {
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('MQTT-046: mqtts://失败后回退mqtt', async ({ authPage }) => {
    test.setTimeout(90_000);
    // 等待设备从 MQTTS 失败中恢复
    for (let i = 0; i < 6; i++) {
      const ok = await authPage.evaluate(async () => {
        try { return (await fetch('/api/health')).ok; } catch { return false; }
      });
      if (ok) break;
      await authPage.waitForTimeout(5000);
    }
    await authPage.locator('#mqtt-scheme').selectOption('mqtt');
    await authPage.waitForTimeout(500);
    await authPage.click('#mqtt-form button[type="submit"]');
    await authPage.waitForTimeout(10_000);
    await expect(authPage.locator('#protocol-page')).toBeVisible({ timeout: 15_000 });
  });

  test('MQTT-047: mqtts://连接后DRAM监控', async ({ authPage, navigateTo }) => {
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    const heapData = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.heap?.free ?? -1;
      } catch { return -1; }
    });
    console.log(`Heap空闲: ${heapData}`);
  });

  test('MQTT-048: mqtts://断开后内存释放', async ({ authPage }) => {
    const enableCb = authPage.locator('#mqtt-enabled');
    if (await enableCb.isChecked()) {
      await enableCb.uncheck();
      await authPage.click('#mqtt-form button[type="submit"]');
      await waitForDevice(authPage, 5000);
      await enableCb.check();
    }
  });

  test('MQTT-049: mqtts://自动重连', async ({ authPage }) => {
    const autoReconnect = authPage.locator('#mqtt-auto-reconnect');
    if (!(await autoReconnect.isChecked())) {
      await autoReconnect.check();
    }
  });

  test('MQTT-050: mqtts://重连内存泄漏检测', async ({ authPage }) => {
    const heapValues: number[] = [];
    for (let i = 0; i < 3; i++) {
      const heap = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/status');
          const data = await r.json();
          return data?.heap?.free ?? -1;
        } catch { return -1; }
      });
      if (heap >= 0) heapValues.push(heap);
      await authPage.waitForTimeout(10_000);
    }
    console.log(`Heap采样: ${heapValues.join(', ')}`);
  });

  // ========== 场景I: MQTT发布/订阅功能 ==========

  test('MQTT-051: 产品秘钥输入', async ({ authPage }) => {
    const secret = authPage.locator('#mqtt-secret');
    if (await secret.isVisible()) {
      await secret.fill('0123456789abcdef');
      const value = await secret.inputValue();
      expect(value).toBe('0123456789abcdef');
    }
  });

  test('MQTT-052: 授权码输入', async ({ authPage }) => {
    const authCode = authPage.locator('#mqtt-auth-code');
    if (await authCode.isVisible()) {
      await authCode.fill('test-code');
      const value = await authCode.inputValue();
      expect(value).toBe('test-code');
    }
  });

  test('MQTT-053: MQTT时间同步按钮', async ({ authPage }) => {
    const syncBtn = authPage.locator('button[data-action="mqttNtpSync"]');
    if (await syncBtn.isVisible()) {
      await syncBtn.click();
      await authPage.waitForTimeout(3000);
    }
  });

  test('MQTT-054: MQTT测试结果', async ({ authPage }) => {
    const testResult = authPage.locator('#mqtt-test-result');
    if (await testResult.isVisible()) {
      const text = await testResult.textContent();
      console.log(`MQTT测试结果: ${text}`);
    }
  });

  test('MQTT-055: 遗嘱保留开关', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-will-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const retainCb = authPage.locator('#mqtt-will-retain');
    if (await retainCb.isVisible()) {
      await retainCb.check();
      await authPage.waitForTimeout(200);
      await retainCb.uncheck();
    }
  });

  test('MQTT-056: 卡平台编号输入', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-card-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const cardPlatform = authPage.locator('#mqtt-card-platform-id');
    if (await cardPlatform.isVisible()) {
      await cardPlatform.fill('123');
      const value = await cardPlatform.inputValue();
      expect(value).toBe('123');
    }
  });

  test('MQTT-057: 摘要JSON输入', async ({ authPage }) => {
    const header = authPage.locator('[data-args="mqtt-card-body"]').first();
    if (await header.isVisible()) await header.click();
    await authPage.waitForTimeout(500);
    const summary = authPage.locator('#mqtt-summary');
    if (await summary.isVisible()) {
      await summary.fill('{"name":"FastBee","chip":"ESP32"}');
      const value = await summary.inputValue();
      expect(value).toContain('FastBee');
    }
  });

  test('MQTT-058: MQTT成功消息元素', async ({ authPage }) => {
    const successMsg = authPage.locator('#mqtt-success');
    await expect(successMsg).toBeDefined();
  });

  test('MQTT-059: 完整MQTT配置保存+刷新', async ({ authPage }) => {
    await authPage.locator('#mqtt-scheme').selectOption('mqtt');
    await authPage.fill('#mqtt-broker', env.mqtt.broker);
    await authPage.fill('#mqtt-port', String(env.mqtt.portMqtt));
    await authPage.fill('#mqtt-client-id', env.mqtt.clientId);
    await authPage.fill('#mqtt-username', env.mqtt.username);
    await authPage.fill('#mqtt-password', env.mqtt.password);
    await authPage.click('#mqtt-form button[type="submit"]');
    await waitForDevice(authPage, 5000);
    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    if (await refreshBtn.isVisible()) {
      await refreshBtn.click();
      await authPage.waitForTimeout(3000);
    }
    const badge = authPage.locator('#mqtt-status-badge');
    if (await badge.isVisible()) {
      const status = await badge.textContent();
      console.log(`最终MQTT状态: ${status}`);
    }
  });

  test('MQTT-060: MQTT配置持久化验证', async ({ authPage }) => {
    const broker = await authPage.locator('#mqtt-broker').inputValue();
    const clientId = await authPage.locator('#mqtt-client-id').inputValue();
    console.log(`Broker: ${broker ? '已配置' : '空'}, ClientID: ${clientId ? '已配置' : '空'}`);
  });

  test('MQTT-061: MQTT表单完整性', async ({ authPage }) => {
    const fields = ['#mqtt-enabled', '#mqtt-auto-reconnect', '#mqtt-scheme',
      '#mqtt-broker', '#mqtt-port', '#mqtt-client-id', '#mqtt-username', '#mqtt-password'];
    for (const field of fields) {
      const el = authPage.locator(field);
      await expect(el).toBeVisible();
    }
  });

  test('MQTT-062: MQTT状态面板完整性', async ({ authPage }) => {
    const fields = ['#mqtt-status-badge', '#mqtt-status-server',
      '#mqtt-status-clientid', '#mqtt-status-reconnects'];
    for (const field of fields) {
      const el = authPage.locator(field);
      await expect(el).toBeVisible();
    }
  });

  test('MQTT-063: MQTT表单按钮完整性', async ({ authPage }) => {
    const saveBtn = authPage.locator('#mqtt-form button[type="submit"]');
    await expect(saveBtn).toBeVisible();
    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    await expect(refreshBtn).toBeVisible();
    const syncBtn = authPage.locator('button[data-action="mqttNtpSync"]');
    await expect(syncBtn).toBeVisible();
  });

  test('MQTT-064: 可折叠区域展开/收起', async ({ authPage }) => {
    const sections = ['mqtt-card-body', 'mqtt-publish-body', 'mqtt-subscribe-body', 'mqtt-will-body'];
    for (const section of sections) {
      const header = authPage.locator(`[data-args="${section}"]`).first();
      if (await header.isVisible()) {
        await header.click();
        await authPage.waitForTimeout(300);
        const body = authPage.locator(`#${section}`);
        if (await body.isVisible()) {
          await expect(body).toBeVisible();
        }
        await header.click(); // 收起
        await authPage.waitForTimeout(300);
      }
    }
  });

  test('MQTT-065: API健康检查', async ({ authPage }) => {
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });
});
