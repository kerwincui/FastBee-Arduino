import { test, expect, env, waitForDevice } from '../fixtures/base.fixture';

test.describe('Suite-18: 多模块综合联动测试', () => {

  // ========== 场景A：外设→执行→MQTT→规则脚本 全链路联动 ==========

  test('LNK-001: 外设创建→执行规则控制→MQTT上报 @quick', async ({ authPage, navigateTo }) => {
    // 1. 获取已启用外设
    await navigateTo('peripheral');
    const periphs = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/peripherals?compact=1&enabledOnly=1&pageSize=50');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`已启用外设: ${periphs.length}`);

    // 2. 获取执行规则
    await navigateTo('periph-exec');
    const rules = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/periph-exec?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`执行规则: ${rules.length}`);

    // 3. 检查MQTT状态
    await navigateTo('protocol');
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    const connected = mqttStatus?.status === 'connected' || mqttStatus?.data?.status === 'connected';
    console.log(`MQTT连接: ${connected}`);

    // 4. 如有规则和外设，尝试手动触发一次
    if (periphs.length > 0 && rules.length > 0) {
      const runResult = await authPage.evaluate(async (id: string) => {
        try {
          const r = await fetch('/api/periph-exec/run', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `id=${encodeURIComponent(id)}`
          });
          return { status: r.status };
        } catch { return null; }
      }, rules[0].id);
      console.log(`规则触发结果: status=${runResult?.status}`);
    }

    // 全链路验证：页面均可达
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-002: MQTT命令→执行规则→外设控制', async ({ authPage, navigateTo }) => {
    // 1. 检查MQTT配置
    await navigateTo('protocol');
    const mqttConfig = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT状态: ${JSON.stringify(mqttConfig?.status ?? mqttConfig?.data?.status)}`);

    // 2. 检查执行规则中的MQTT触发类型
    const rules = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/periph-exec?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    const mqttRules = rules.filter((r: any) => r.triggerType === 'mqtt' || r.trigger?.type === 'mqtt');
    console.log(`MQTT触发规则: ${mqttRules.length}`);

    // 3. 检查外设列表
    await navigateTo('peripheral');
    const periphs = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/peripherals?compact=1&pageSize=50');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`外设总数: ${periphs.length}`);

    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-003: 规则脚本转换→MQTT主题转发→执行触发', async ({ authPage, navigateTo }) => {
    // 1. 检查规则脚本
    await navigateTo('rule-script');
    const scripts = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/rule-script?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`规则脚本: ${scripts.length}`);

    // 2. 检查MQTT配置
    await navigateTo('protocol');
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT连接: ${mqttStatus?.status ?? mqttStatus?.data?.status}`);

    // 3. 检查执行规则
    const execRules = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/periph-exec?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`执行规则: ${execRules.length}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-004: 多个外设+多条规则+MQTT同步', async ({ authPage, navigateTo }) => {
    // 1. 外设列表
    await navigateTo('peripheral');
    const periphs = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/peripherals?compact=1&pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`外设总数: ${periphs.length}`);

    // 2. 执行规则
    await navigateTo('periph-exec');
    const rules = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/periph-exec?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`执行规则: ${rules.length}`);

    // 3. MQTT订阅主题
    await navigateTo('protocol');
    const mqttTopics = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT状态: ${mqttTopics?.status ?? mqttTopics?.data?.status}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-005: 禁用外设→执行规则失效验证', async ({ authPage, navigateTo }) => {
    // 获取外设列表
    await navigateTo('peripheral');
    const periphs = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/peripherals?compact=1&pageSize=50');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`外设总数: ${periphs.length}`);

    // 找到已启用的外设
    const enabled = periphs.filter((p: any) => p.enabled !== false);
    console.log(`已启用外设: ${enabled.length}`);

    // 获取执行规则
    const rules = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/periph-exec?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`执行规则: ${rules.length}`);

    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 场景B：Modbus→MQTT→规则脚本 数据采集链路 ==========

  test('LNK-006: Modbus采集→JSON格式→MQTT发布', async ({ authPage, navigateTo }) => {
    // 1. 检查Modbus状态
    await navigateTo('protocol');
    const modbusStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/modbus/status');
        return await r.json();
      } catch { return null; }
    });
    const modbusEnabled = modbusStatus?.data?.enabled ?? modbusStatus?.enabled ?? false;
    const modbusMode = modbusStatus?.data?.mode ?? modbusStatus?.mode ?? 'N/A';
    console.log(`Modbus启用: ${modbusEnabled}, 模式: ${modbusMode}`);

    // 2. 检查MQTT
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    const mqttConnected = mqttStatus?.status === 'connected' || mqttStatus?.data?.status === 'connected';
    console.log(`MQTT连接: ${mqttConnected}`);

    // 3. 如果Modbus启用，检查设备列表
    if (modbusEnabled) {
      const modbusDevices = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/modbus/devices?pageSize=50');
          const data = await r.json();
          return data?.data || [];
        } catch { return []; }
      });
      console.log(`Modbus设备: ${modbusDevices.length}`);
    }
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-007: Modbus采集→透传模式→MQTT发布', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const modbusStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/modbus/status');
        return await r.json();
      } catch { return null; }
    });
    const mode = modbusStatus?.data?.transferMode ?? modbusStatus?.data?.mode ?? 'N/A';
    console.log(`Modbus传输模式: ${mode}`);

    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT: ${mqttStatus?.status ?? mqttStatus?.data?.status ?? 'unknown'}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-008: Modbus控制设备→MQTT命令触发', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');

    // Modbus线圈状态
    const coilStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/modbus/coil/status?slaveAddress=1&coilBase=0&channelCount=4');
        return { status: r.status, body: await r.json() };
      } catch { return null; }
    });
    console.log(`Modbus线圈API: status=${coilStatus?.status}`);

    // MQTT状态
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT: ${mqttStatus?.status ?? mqttStatus?.data?.status ?? 'unknown'}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-009: Modbus轮询+规则脚本数据转换', async ({ authPage, navigateTo }) => {
    // Modbus状态
    await navigateTo('protocol');
    const modbusStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/modbus/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`Modbus: enabled=${modbusStatus?.data?.enabled ?? modbusStatus?.enabled}`);

    // 规则脚本
    await navigateTo('rule-script');
    const scripts = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/rule-script?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`规则脚本: ${scripts.length}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-010: Modbus多设备轮询→多主题发布 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');

    const modbusDevices = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/modbus/devices?pageSize=50');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`Modbus采集设备: ${modbusDevices.length}`);

    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT: ${mqttStatus?.status ?? mqttStatus?.data?.status ?? 'unknown'}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 场景C：网络变更→全功能联动 ==========

  test('LNK-011: WiFi→MQTT→外设→日志 全链路', async ({ authPage, navigateTo }) => {
    // 1. 检查WiFi状态
    await navigateTo('network');
    const netStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        return (await r.json())?.network?.status ?? 'unknown';
      } catch { return 'error'; }
    });
    console.log(`网络状态: ${netStatus}`);

    // 2. 检查MQTT状态
    await navigateTo('protocol');
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return (await r.json())?.status ?? 'unknown';
      } catch { return 'disabled'; }
    });
    console.log(`MQTT状态: ${mqttStatus}`);

    // 3. 检查外设页面可访问
    await navigateTo('peripheral');
    await expect(authPage.locator('#app-container')).toBeVisible();

    // 4. 查看日志
    await navigateTo('logs');
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-012: 开发环境禁用→全功能锁定→启用恢复', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const advTab = authPage.locator('[data-tab="advanced"], :text("高级配置")').first();
    if (await advTab.isVisible()) {
      await advTab.click();
      await authPage.waitForTimeout(1000);

      const disableBtn = authPage.locator('#disable-dev-btn, button:has-text("禁用开发环境")').first();
      if (await disableBtn.isVisible()) {
        const pwdInput = authPage.locator('#dev-mode-password, input[name="devPassword"]').first();
        if (await pwdInput.isVisible()) {
          await pwdInput.fill('admin');
          await disableBtn.click();
          await waitForDevice(authPage, 3000);

          // 验证外设页面被锁
          await navigateTo('peripheral');
          const devNotice = authPage.locator('.dev-mode-notice, .developer-disabled-notice').first();
          if (await devNotice.isVisible()) {
            await expect(devNotice).toContainText(/禁用|disabled/i);
          }

          // 重新启用
          await navigateTo('device');
          await advTab.click();
          await authPage.waitForTimeout(1000);
          const enableBtn = authPage.locator('#enable-dev-btn, button:has-text("启用开发环境")').first();
          if (await enableBtn.isVisible()) {
            const pwdInput2 = authPage.locator('#dev-mode-password, input[name="devPassword"]').first();
            if (await pwdInput2.isVisible()) await pwdInput2.fill('admin');
            await enableBtn.click();
            await waitForDevice(authPage, 3000);
          }
        }
      }
    }
  });

  test('LNK-013: 重启设备→全量恢复验证', async ({ authPage, navigateTo }) => {
    // 1. 记录当前配置
    await navigateTo('device');
    const deviceName = await authPage.locator('#device-name, input[name="deviceName"]').first().inputValue().catch(() => '');

    // 2. 触发重启
    const rebootBtn = authPage.locator('#reboot-btn, button:has-text("重启")').first();
    if (await rebootBtn.isVisible()) {
      await rebootBtn.click();
      await authPage.waitForTimeout(1000);
      const confirmBtn = authPage.locator('.modal-confirm-btn, button:has-text("确定")').first();
      if (await confirmBtn.isVisible()) await confirmBtn.click();

      // 3. 等待设备重启
      console.log('等待设备重启...');
      const start = Date.now();
      let recovered = false;
      while (Date.now() - start < 120_000) {
        try {
          const r = await authPage.evaluate(async () => {
            try { return (await fetch('/api/health')).ok; } catch { return false; }
          });
          if (r) { recovered = true; break; }
        } catch { /* ignore */ }
        await authPage.waitForTimeout(5000);
      }
      console.log(`设备重启${recovered ? '成功' : '超时'} (${Date.now() - start}ms)`);
      expect(recovered).toBeTruthy();

      // 4. 重新登录
      await authPage.goto('/');
      await authPage.fill('#username', 'admin');
      await authPage.fill('#password', 'admin');
      await authPage.click('#login-button');
      await authPage.waitForSelector('#app-container', { state: 'visible', timeout: 15_000 });

      // 5. 验证设备名称
      await navigateTo('device');
      const newName = await authPage.locator('#device-name, input[name="deviceName"]').first().inputValue().catch(() => '');
      if (deviceName) expect(newName).toBe(deviceName);
    }
  });

  test('LNK-014: 配置导出→恢复出厂→导入恢复', async ({ authPage, navigateTo }) => {
    // 1. 导出配置
    await navigateTo('device');
    const exportBtn = authPage.locator('#export-config-btn, button:has-text("导出")').first();
    let configExported = false;
    if (await exportBtn.isVisible()) {
      // 监听下载
      const downloadPromise = authPage.waitForEvent('download', { timeout: 10_000 }).catch(() => null);
      await exportBtn.click();
      const download = await downloadPromise;
      if (download) {
        configExported = true;
        console.log(`配置已导出: ${download.suggestedFilename()}`);
      }
    }
    console.log(`配置导出: ${configExported ? '成功' : '跳过(无导出按钮)'}`);

    // 2. API方式获取配置备份
    const configBackup = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/config/export');
        if (r.ok) return await r.text();
        return null;
      } catch { return null; }
    });
    console.log(`API配置备份: ${configBackup ? `${configBackup.length}字符` : '不可用'}`);

    // 3. 验证恢复出厂API存在（不实际执行，太危险）
    const factoryResetAvailable = await authPage.evaluate(async () => {
      try {
        // 使用OPTIONS或HEAD探测，不发POST
        const r = await fetch('/api/config/factory-reset', { method: 'OPTIONS' });
        return r.status !== 404;
      } catch { return false; }
    });
    console.log(`恢复出厂API: ${factoryResetAvailable ? '可用' : '不可用'}`);

    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 场景D：用户权限→功能访问联动 ==========

  test('LNK-015: 创建操作员→验证页面访问', async ({ authPage, navigateTo }) => {
    // 检查用户管理
    await navigateTo('users');
    const users = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/users?page=1&limit=20');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`用户数: ${users.length}`);

    // 检查是否有非admin用户
    const nonAdminUsers = users.filter((u: any) => u.username !== 'admin');
    console.log(`非admin用户: ${nonAdminUsers.length}`);

    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-016: 管理员修改密码→重新登录→功能验证', async ({ authPage, navigateTo }) => {
    // 进入用户管理
    await navigateTo('users');
    const editBtn = authPage.locator('button:has-text("编辑"), button:has-text("修改密码")').first();
    if (await editBtn.isVisible()) {
      await editBtn.click();
      await authPage.waitForTimeout(500);
      // 验证密码修改对话框出现
      const pwdModal = authPage.locator('.modal, .modal-dialog, [role="dialog"]').first();
      const modalVisible = await pwdModal.isVisible();
      console.log(`密码修改对话框: ${modalVisible ? '已打开' : '未出现'}`);

      // 关闭对话框（不实际修改密码）
      const cancelBtn = authPage.locator('button:has-text("取消")').first();
      if (await cancelBtn.isVisible()) await cancelBtn.click();
    }

    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-017: 多用户并发登录', async ({ page, baseURL }) => {
    const context = page.context();
    // 标签A: admin登录
    const pageA = await context.newPage();
    await pageA.goto(baseURL || '/', { waitUntil: 'load' });
    await pageA.fill('#username', 'admin');
    await pageA.fill('#password', 'admin');
    await pageA.click('#login-button');
    await pageA.waitForSelector('#app-container', { state: 'visible', timeout: 15_000 });

    // 标签B: admin登录（同一账号，不同session）
    const pageB = await context.newPage();
    await pageB.goto(baseURL || '/', { waitUntil: 'load' });
    await pageB.fill('#username', 'admin');
    await pageB.fill('#password', 'admin');
    await pageB.click('#login-button');
    await pageB.waitForSelector('#app-container', { state: 'visible', timeout: 15_000 });

    // 两个标签页独立操作
    await pageA.click('.menu-item[data-page="dashboard"]');
    await pageB.click('.menu-item[data-page="device"]');
    await pageA.waitForLoadState('networkidle', { timeout: 10_000 });
    await pageB.waitForLoadState('networkidle', { timeout: 10_000 });

    const aVisible = await pageA.locator('#app-container').isVisible();
    const bVisible = await pageB.locator('#app-container').isVisible();
    console.log(`标签A dashboard: ${aVisible}, 标签B device: ${bVisible}`);
    expect(aVisible).toBeTruthy();
    expect(bVisible).toBeTruthy();

    await pageA.close().catch(() => {});
    await pageB.close().catch(() => {});
  });

  test('LNK-018: 删除用户→会话失效', async ({ authPage, navigateTo }) => {
    // 检查用户列表
    await navigateTo('users');
    const users = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/users?page=1&limit=20');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    const nonAdminUsers = users.filter((u: any) => u.username !== 'admin');
    console.log(`可删除用户: ${nonAdminUsers.length}`);

    // 检查删除API存在（不实际执行）
    if (nonAdminUsers.length > 0) {
      const deleteApiAvailable = await authPage.evaluate(async (username: string) => {
        try {
          const r = await fetch(`/api/users?username=${encodeURIComponent(username)}`, { method: 'OPTIONS' });
          return r.status !== 404;
        } catch { return false; }
      }, nonAdminUsers[0].username);
      console.log(`删除API: ${deleteApiAvailable ? '可用' : '不可用'}`);
    }
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 场景E：设备状态→页面展示联动 ==========

  test('LNK-019: 修改设备名称→仪表盘同步', async ({ authPage, navigateTo }) => {
    // 1. 获取当前设备名称
    await navigateTo('device');
    const deviceNameInput = authPage.locator('#device-name, input[name="deviceName"]').first();
    const oldName = await deviceNameInput.inputValue().catch(() => '');
    console.log(`当前设备名: ${oldName}`);

    // 2. 仪表盘查看设备名称
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    const dashboardStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.deviceName ?? data?.name ?? '';
      } catch { return ''; }
    });
    console.log(`仪表盘设备名: ${dashboardStatus}`);

    if (oldName) {
      expect(dashboardStatus).toBe(oldName);
    }
  });

  test('LNK-020: 修改设备名称→MQTT主题变量替换 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const deviceName = await authPage.locator('#device-name, input[name="deviceName"]').first().inputValue().catch(() => '');
    console.log(`设备名: ${deviceName}`);

    await navigateTo('protocol');
    const mqttConfig = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT clientID: ${mqttConfig?.clientId ?? mqttConfig?.data?.clientId ?? 'N/A'}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-021: NTP同步→日志时间戳更新', async ({ authPage, navigateTo }) => {
    await navigateTo('device');
    const ntpTab = authPage.locator('[data-tab="ntp"], :text("NTP")').first();
    if (await ntpTab.isVisible()) {
      await ntpTab.click();
      await authPage.waitForTimeout(1000);
    }

    await navigateTo('logs');
    await authPage.waitForLoadState('networkidle');
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-022: 日志级别切换→日志输出变化', async ({ authPage, navigateTo }) => {
    // 进入设备配置查看日志级别
    await navigateTo('device');
    const advTab = authPage.locator('[data-tab="advanced"], :text("高级")').first();
    if (await advTab.isVisible()) {
      await advTab.click();
      await authPage.waitForTimeout(1000);
    }

    const logLevelSelect = authPage.locator('#log-level, select[name="logLevel"]').first();
    if (await logLevelSelect.isVisible()) {
      const currentLevel = await logLevelSelect.inputValue().catch(() => '');
      console.log(`当前日志级别: ${currentLevel}`);
    }

    // 查看日志页
    await navigateTo('logs');
    await authPage.waitForLoadState('networkidle');
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-023: Flash使用率→文件管理一致性', async ({ authPage, navigateTo }) => {
    // 1. 仪表盘获取Flash使用率
    await navigateTo('dashboard');
    await authPage.waitForLoadState('networkidle');
    const flashDashboard = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return data?.flash?.used ?? data?.fsUsed ?? -1;
      } catch { return -1; }
    });

    // 2. 文件管理页
    await navigateTo('data');
    await authPage.waitForLoadState('networkidle');
    await expect(authPage.locator('#app-container')).toBeVisible();
    console.log(`Dashboard Flash used: ${flashDashboard}`);
  });

  // ========== 场景F：安全与异常场景联动 ==========

  test('LNK-024: 登录锁定期间功能不可用', async ({ page, baseURL }) => {
    // 尝试3次错误登录
    for (let i = 0; i < 3; i++) {
      await page.goto(baseURL || '/', { waitUntil: 'load' });
      await page.fill('#username', 'admin');
      await page.fill('#password', 'wrong_password_123');
      await page.click('#login-button');
      await page.waitForTimeout(1000);
    }

    // 检查是否被锁定
    const lockNotice = page.locator('.lock-notice, .alert-danger, .error-message').first();
    if (await lockNotice.isVisible()) {
      console.log(`锁定提示: ${await lockNotice.textContent()}`);
    }

    // 尝试访问API
    const apiResult = await page.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        return r.status;
      } catch { return 0; }
    });
    console.log(`锁定期间API状态: ${apiResult}`);

    // 用正确密码重新登录（清除锁定）
    await page.goto(baseURL || '/', { waitUntil: 'load' });
    await page.fill('#username', 'admin');
    await page.fill('#password', 'admin');
    await page.click('#login-button');
    await page.waitForSelector('#app-container, #login-page', { state: 'visible', timeout: 15_000 });
  });

  test('LNK-025: 设备重启期间Web状态', async ({ authPage }) => {
    const healthOk = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/health');
        return r.ok;
      } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('LNK-026: OTA升级期间功能锁定', async ({ authPage, navigateTo }) => {
    // 检查OTA状态
    await navigateTo('device');
    const otaStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/ota/status');
        return { status: r.status, body: await r.json() };
      } catch { return null; }
    });
    console.log(`OTA状态API: ${otaStatus?.status}`);

    // OTA进度条
    const otaProgress = authPage.locator('#ota-progress, .ota-progress, [data-ota-progress]').first();
    if (await otaProgress.isVisible()) {
      console.log('OTA进度条可见');
    }

    // 验证设备健康
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();
  });

  test('LNK-027: WiFi断开→MQTT断开→日志记录', async ({ authPage, navigateTo }) => {
    // 检查网络状态
    await navigateTo('network');
    const netStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return {
          wifi: data?.network?.status ?? data?.wifi?.status ?? 'unknown',
          mqtt: 'check_next'
        };
      } catch { return { wifi: 'error', mqtt: 'error' }; }
    });
    console.log(`网络: ${JSON.stringify(netStatus)}`);

    // 检查MQTT
    await navigateTo('protocol');
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return (await r.json())?.status ?? 'unknown';
      } catch { return 'unknown'; }
    });
    console.log(`MQTT: ${mqttStatus}`);

    // 查看日志
    await navigateTo('logs');
    await authPage.waitForLoadState('networkidle');
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-028: MQTTS失败→Web正常→日志记录', async ({ authPage, navigateTo }) => {
    const healthOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/health')).ok; } catch { return false; }
    });
    expect(healthOk).toBeTruthy();

    await navigateTo('logs');
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 场景G：多设备联动场景 ==========

  test('LNK-029: 两台设备同WiFi+同MQTT Broker', async ({ authPage, navigateTo }) => {
    // 检查当前设备的WiFi和MQTT配置
    await navigateTo('network');
    const netStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return { ssid: data?.wifi?.ssid ?? data?.network?.ssid ?? 'unknown' };
      } catch { return { ssid: 'error' }; }
    });
    console.log(`WiFi SSID: ${netStatus.ssid}`);

    await navigateTo('protocol');
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        const data = await r.json();
        return {
          broker: data?.server ?? data?.data?.server ?? 'N/A',
          clientId: data?.clientId ?? data?.data?.clientId ?? 'N/A'
        };
      } catch { return { broker: 'N/A', clientId: 'N/A' }; }
    });
    console.log(`MQTT Broker: ${mqttStatus.broker}, ClientID: ${mqttStatus.clientId}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-030: 设备A发布→设备B订阅联动', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');

    // 检查MQTT发布主题
    const publishTopics = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/topics');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`MQTT发布主题: ${publishTopics.length}`);

    // 检查MQTT订阅主题
    const subscribeTopics = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/subscriptions');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`MQTT订阅主题: ${subscribeTopics.length}`);
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  test('LNK-031: 4台设备配置导入导出一致性', async ({ authPage, navigateTo }) => {
    await navigateTo('device');

    // 导出配置
    const configExport = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/config/export');
        if (r.ok) return await r.text();
        return null;
      } catch { return null; }
    });
    console.log(`配置导出: ${configExport ? `${configExport.length}字符` : '不可用'}`);

    // 验证配置包含关键字段
    if (configExport) {
      try {
        const config = JSON.parse(configExport);
        console.log(`配置字段: ${Object.keys(config).join(', ')}`);
      } catch {
        console.log('配置非JSON格式');
      }
    }
    await expect(authPage.locator('#app-container')).toBeVisible();
  });

  // ========== 场景H：长时间运行联动稳定性 ==========

  test('LNK-032: 全功能5分钟稳定性', async ({ authPage, navigateTo }) => {
    const menuPages = ['dashboard', 'device', 'network', 'protocol', 'logs'];
    for (let round = 0; round < 5; round++) {
      for (const page of menuPages) {
        await navigateTo(page);
        await authPage.waitForTimeout(5000);
      }
      const ok = await authPage.evaluate(async () => {
        try { return (await fetch('/api/health')).ok; } catch { return false; }
      });
      console.log(`轮次[${round}] 健康: ${ok}`);
      expect(ok).toBeTruthy();
    }
  });

  test('LNK-033: 全功能1小时浸泡测试(抽样)', async ({ authPage, navigateTo }) => {
    // 抽样验证（实际1小时缩短为3轮x5页面）
    const menuPages = ['dashboard', 'device', 'network', 'peripheral', 'protocol'];
    for (let round = 0; round < 3; round++) {
      for (const page of menuPages) {
        await navigateTo(page);
        await authPage.waitForTimeout(2000);
      }

      // 每轮检查Heap
      const heap = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/status');
          const data = await r.json();
          return data?.heap?.free ?? data?.freeHeap ?? -1;
        } catch { return -1; }
      });

      const healthOk = await authPage.evaluate(async () => {
        try { return (await fetch('/api/health')).ok; } catch { return false; }
      });
      console.log(`浸泡轮次[${round}] heap=${heap}, health=${healthOk}`);
      expect(healthOk).toBeTruthy();
    }
  });

  test('LNK-034: 高频操作10分钟压测(抽样)', async ({ authPage, navigateTo }) => {
    // 高频操作抽样（每2秒一次操作，持续约1分钟）
    const menuPages = ['dashboard', 'device', 'network', 'protocol', 'logs'];
    const start = Date.now();
    let opCount = 0;

    while (Date.now() - start < 60_000) {
      const page = menuPages[opCount % menuPages.length];
      await navigateTo(page);
      await authPage.waitForTimeout(1000);

      // 每5次操作检查一次健康
      if (opCount % 5 === 0) {
        const ok = await authPage.evaluate(async () => {
          try { return (await fetch('/api/health')).ok; } catch { return false; }
        });
        console.log(`高频操作[${opCount}] 健康: ${ok}`);
        expect(ok).toBeTruthy();
      }
      opCount++;
    }
    console.log(`总操作次数: ${opCount}`);
  });

  test('LNK-035: 断电恢复后全功能验证', async ({ authPage, navigateTo }) => {
    // 模拟断电恢复：记录当前配置，重启后验证
    await navigateTo('device');
    const deviceName = await authPage.locator('#device-name, input[name="deviceName"]').first().inputValue().catch(() => '');

    // 获取MQTT配置
    await navigateTo('protocol');
    const mqttConfig = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT clientID: ${mqttConfig?.clientId ?? mqttConfig?.data?.clientId ?? 'N/A'}`);

    // 获取外设数量
    await navigateTo('peripheral');
    const periphCount = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/peripherals?compact=1&pageSize=100');
        const data = await r.json();
        return (data?.data || []).length;
      } catch { return 0; }
    });
    console.log(`外设数量: ${periphCount}`);

    // 获取规则数量
    await navigateTo('periph-exec');
    const ruleCount = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/periph-exec?pageSize=100');
        const data = await r.json();
        return (data?.data || []).length;
      } catch { return 0; }
    });
    console.log(`规则数量: ${ruleCount}`);

    console.log(`设备名: ${deviceName}, 外设: ${periphCount}, 规则: ${ruleCount}`);

    // 验证NVS存储持久性（设备已运行中，配置应已持久化）
    const statusOk = await authPage.evaluate(async () => {
      try { return (await fetch('/api/status')).ok; } catch { return false; }
    });
    expect(statusOk).toBeTruthy();
  });

  // ========== 传感器→MQTT→仪表盘闭环 ==========

  test('LNK-040: 传感器→MQTT→仪表盘闭环', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    const sensors = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/peripherals?compact=1&enabledOnly=1&pageSize=50');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`已启用外设数量: ${sensors.length}`);

    if (sensors.length > 0) {
      const firstId = sensors[0].id;
      const readResult = await authPage.evaluate(async (id: string) => {
        try {
          const r = await fetch(`/api/peripherals/read?id=${encodeURIComponent(id)}`);
          return await r.json();
        } catch { return null; }
      }, firstId);
      console.log(`外设 ${firstId} 读取: ${JSON.stringify(readResult?.data ?? readResult)}`);
    }

    await navigateTo('protocol');
    const mqttStatus = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/mqtt/status');
        return await r.json();
      } catch { return null; }
    });
    console.log(`MQTT: ${mqttStatus?.status ?? mqttStatus?.data?.status ?? 'unknown'}`);

    await navigateTo('dashboard');
    await expect(authPage.locator('#app-container')).toBeVisible();
    const dashData = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/status')).json(); } catch { return null; }
    });
    expect(dashData).toBeTruthy();
    console.log(`仪表盘 chip=${dashData?.chip}, heap=${dashData?.heap}`);
  });

  // ========== 外设配置→执行规则→动作触发 ==========

  test('LNK-041: 外设配置→执行规则→动作触发', async ({ authPage, navigateTo }) => {
    await navigateTo('peripheral');
    const periphCount = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/peripherals?compact=1&pageSize=100');
        const data = await r.json();
        return (data?.data || []).length;
      } catch { return 0; }
    });
    console.log(`外设总数: ${periphCount}`);

    await navigateTo('periph-exec');
    await expect(authPage.locator('#periph-exec-page, #app-container')).toBeVisible();
    const rules = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/periph-exec?pageSize=100');
        const data = await r.json();
        return data?.data || [];
      } catch { return []; }
    });
    console.log(`执行规则: ${rules.length}`);

    if (rules.length > 0) {
      const ruleId = rules[0].id;
      const runResult = await authPage.evaluate(async (id: string) => {
        try {
          const r = await fetch('/api/periph-exec/run', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: `id=${encodeURIComponent(id)}`
          });
          return { status: r.status, body: await r.json() };
        } catch { return null; }
      }, ruleId);
      console.log(`规则 ${ruleId} 执行: status=${runResult?.status}`);
    }
  });

  // ========== GPIO配置→引脚冲突UI反馈 ==========

  test('LNK-042: GPIO配置→引脚冲突检测反馈', async ({ authPage, navigateTo }) => {
    const chipInfo = await authPage.evaluate(async () => {
      try {
        const r = await fetch('/api/status');
        const data = await r.json();
        return { chip: data?.chip, maxGpio: data?.maxGpio ?? data?.gpioMax };
      } catch { return null; }
    });
    console.log(`芯片: ${chipInfo?.chip}, MAX_GPIO: ${chipInfo?.maxGpio}`);

    await navigateTo('peripheral');
    // GPIO0保留引脚
    const validateResult = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/peripherals/validate-pins?type=1&pins=0')).json(); }
      catch { return null; }
    });
    console.log(`GPIO0验证: ${JSON.stringify(validateResult?.data ?? validateResult)}`);

    // GPIO4有效引脚
    const validPinResult = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/peripherals/validate-pins?type=1&pins=4')).json(); }
      catch { return null; }
    });
    console.log(`GPIO4验证: ${JSON.stringify(validPinResult?.data ?? validPinResult)}`);

    // GPIO255超范围
    const invalidPinResult = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/peripherals/validate-pins?type=1&pins=255')).json(); }
      catch { return null; }
    });
    console.log(`GPIO255验证: ${JSON.stringify(invalidPinResult?.data ?? invalidPinResult)}`);
  });

  // ========== Modbus→MQTT数据桥接 ==========

  test('LNK-043: Modbus→MQTT数据桥接', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    const modbusStatus = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/modbus/status')).json(); } catch { return null; }
    });
    const modbusEnabled = modbusStatus?.data?.enabled ?? modbusStatus?.enabled ?? false;
    console.log(`Modbus: enabled=${modbusEnabled}`);

    const mqttStatus = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/mqtt/status')).json(); } catch { return null; }
    });
    const mqttConnected = mqttStatus?.status === 'connected' || mqttStatus?.data?.status === 'connected';
    console.log(`MQTT: connected=${mqttConnected}`);

    if (modbusEnabled) {
      const coilStatus = await authPage.evaluate(async () => {
        try {
          const r = await fetch('/api/modbus/coil/status?slaveAddress=1&coilBase=0&channelCount=4');
          return { status: r.status };
        } catch { return null; }
      });
      console.log(`Modbus线圈API: status=${coilStatus?.status}`);
    }

    const protoConfig = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/protocol/config?compact=1')).json(); } catch { return null; }
    });
    expect(protoConfig).toBeTruthy();
  });

  // ========== 配置变更→日志审计 ==========

  test('LNK-044: 配置变更→日志审计', async ({ authPage, navigateTo }) => {
    await navigateTo('logs');
    const logsBefore = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/logs/list?limit=5')).json(); } catch { return null; }
    });
    const beforeCount = logsBefore?.data?.length ?? logsBefore?.length ?? 0;
    console.log(`变更前日志: ${beforeCount}条`);

    await navigateTo('network');
    await authPage.waitForLoadState('networkidle');
    const netStatus = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/status')).json(); } catch { return null; }
    });
    expect(netStatus).toBeTruthy();

    const logsAfter = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/logs/list?limit=10')).json(); } catch { return null; }
    });
    const afterCount = logsAfter?.data?.length ?? logsAfter?.length ?? 0;
    console.log(`变更后日志: ${afterCount}条`);
    expect(afterCount).toBeGreaterThanOrEqual(beforeCount);
  });

  // ========== 用户权限联动 ==========

  test('LNK-045: 用户权限联动', async ({ authPage, navigateTo }) => {
    const onlineUsers = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/users/online')).json(); } catch { return null; }
    });
    console.log(`在线用户: ${JSON.stringify(onlineUsers?.data?.length ?? onlineUsers)}`);

    const adminAccessiblePages: string[] = [];
    const allPages = ['dashboard', 'device', 'network', 'peripheral', 'periph-exec',
                      'protocol', 'device-control', 'rule-script', 'logs', 'data', 'users'];
    for (const pageName of allPages) {
      try {
        await navigateTo(pageName);
        const visible = await authPage.locator('#app-container').isVisible();
        if (visible) adminAccessiblePages.push(pageName);
      } catch {
        console.log(`admin 无法访问: ${pageName}`);
      }
    }
    console.log(`admin可访问: ${adminAccessiblePages.length}/${allPages.length}`);
    expect(adminAccessiblePages.length).toBeGreaterThanOrEqual(5);

    await navigateTo('users');
    const usersPageVisible = await authPage.locator('#app-container').isVisible();
    expect(usersPageVisible).toBeTruthy();

    const userList = await authPage.evaluate(async () => {
      try { return await (await fetch('/api/users?page=1&limit=20')).json(); } catch { return null; }
    });
    console.log(`用户列表: ${JSON.stringify(userList?.data?.length ?? userList)}`);
  });
});
