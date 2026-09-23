import { test, expect } from '../fixtures/base.fixture';

test.describe('Suite-25: MQTT 状态错误反馈', () => {
  test('MQTT-003B: 首次状态返回前显示中性检测态 @quick', async ({ authPage, navigateTo }) => {
    let releaseStatus!: () => void;
    const statusGate = new Promise<void>(resolve => { releaseStatus = resolve; });
    await authPage.route('**/api/mqtt/status', async route => {
      await statusGate;
      await route.fulfill({
        status: 200,
        contentType: 'application/json; charset=utf-8',
        body: JSON.stringify({
          success: true,
          data: {
            enabled: true,
            initialized: true,
            connected: true,
            networkConnected: true,
            server: 'broker.example.com',
            port: 1883,
            clientId: 'test-client',
            reconnectCount: 0,
          },
        }),
      });
    });

    await navigateTo('protocol');
    await expect(authPage.locator('#mqtt-status-badge')).toHaveText('检测中');
    await expect(authPage.locator('#mqtt-status-badge')).toHaveClass(/mqtt-status-detecting/);
    expect(await authPage.locator('#mqtt-status-panel .fb-skeleton-text').count()).toBeGreaterThan(0);

    releaseStatus();
    await expect(authPage.locator('#mqtt-status-badge')).toHaveText('已连接');
    await expect(authPage.locator('#mqtt-status-panel .fb-skeleton-text')).toHaveCount(0);
    await authPage.unroute('**/api/mqtt/status');
  });

  test('MQTT-003A: 连接失败原因明确显示且不修改配置 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    await authPage.locator('#mqtt-enabled').waitFor({ state: 'visible', timeout: 15_000 });

    await authPage.route('**/api/mqtt/status', async route => {
      await route.fulfill({
        status: 200,
        contentType: 'application/json; charset=utf-8',
        body: JSON.stringify({
          success: true,
          data: {
            enabled: true,
            initialized: true,
            connected: false,
            connecting: false,
            stopped: true,
            autoReconnect: false,
            networkConnected: true,
            lastErrorCode: 4,
            lastErrorClass: 'auth',
            lastErrorMessage: 'Broker 拒绝：用户名或密码错误',
          },
        }),
      });
    });

    const refreshBtn = authPage.locator('button[data-action="refreshMqttStatus"]');
    await expect(refreshBtn).toBeVisible();
    await refreshBtn.click();
    await expect(authPage.locator('#mqtt-status-badge')).toHaveText('连接失败');
    await expect(authPage.locator('#mqtt-status-reason')).toContainText('用户名或密码错误');
    await expect(authPage.locator('#mqtt-status-panel .fb-skeleton-text')).toHaveCount(0);
    await expect(authPage.locator('#mqtt-status-server')).toHaveText('--');
    await expect(authPage.locator('#mqtt-status-clientid')).toHaveText('--');
    await expect(authPage.locator('#mqtt-status-reconnects')).toHaveText('0');
  });

  test('MQTT-003C: 连续状态请求失败退出骨架并显示失败 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    await authPage.locator('#mqtt-status-badge').waitFor({ state: 'visible', timeout: 15_000 });

    let requestCount = 0;
    await authPage.route('**/api/mqtt/status', async route => {
      requestCount++;
      await route.fulfill({
        status: 200,
        contentType: 'application/json; charset=utf-8',
        body: JSON.stringify({ success: false, error: '状态服务暂时不可用' }),
      });
    });

    try {
      await authPage.evaluate(() => {
        const state = (window as any).AppState;
        state._stopMqttStatusPolling();
        state._mqttApiFailCount = 0;
        state._setMqttStatusLoading();
        state._loadMqttStatus();
      });
      await expect.poll(() => requestCount).toBe(1);
      await expect.poll(() => authPage.evaluate(() => (window as any).AppState._mqttApiFailCount)).toBe(1);
      await expect(authPage.locator('#mqtt-status-badge')).toHaveText('检测中');
      expect(await authPage.locator('#mqtt-status-panel .fb-skeleton-text').count()).toBeGreaterThan(0);

      await authPage.evaluate(() => {
        const state = (window as any).AppState;
        state._stopMqttStatusPolling();
        state._loadMqttStatus();
      });
      await expect.poll(() => requestCount).toBe(2);
      await expect(authPage.locator('#mqtt-status-badge')).toHaveText('状态获取失败');
      await expect(authPage.locator('#mqtt-status-reason')).toContainText('状态服务暂时不可用');
      await expect(authPage.locator('#mqtt-status-panel .fb-skeleton-text')).toHaveCount(0);
      await expect(authPage.locator('#mqtt-status-server')).toHaveText('--');
      await expect(authPage.locator('#mqtt-status-clientid')).toHaveText('--');
      await expect(authPage.locator('#mqtt-status-reconnects')).toHaveText('0');
    } finally {
      await authPage.evaluate(() => (window as any).AppState._stopMqttStatusPolling());
      await authPage.unroute('**/api/mqtt/status');
    }
  });

  test('MQTT-003D: 旧状态响应不得覆盖新页面结果 @quick', async ({ authPage, navigateTo }) => {
    await navigateTo('protocol');
    await authPage.locator('#mqtt-status-badge').waitFor({ state: 'visible', timeout: 15_000 });

    await authPage.evaluate(() => {
      const target = window as any;
      const state = target.AppState;
      state._stopMqttStatusPolling();
      target.__mqttStatusOriginalGet = target.apiGetSilent;
      target.__mqttStatusResolvers = [];
      target.apiGetSilent = (url: string, params?: unknown) => {
        if (url !== '/api/mqtt/status') return target.__mqttStatusOriginalGet(url, params);
        return new Promise((resolve) => target.__mqttStatusResolvers.push(resolve));
      };
      state._setMqttStatusLoading();
      state._loadMqttStatus();
    });
    await expect.poll(() => authPage.evaluate(() => (window as any).__mqttStatusResolvers.length)).toBe(1);

    try {
      await authPage.evaluate(() => {
        const state = (window as any).AppState;
        state._stopMqttStatusPolling();
        state._setMqttStatusLoading();
        state._loadMqttStatus();
      });
      await expect.poll(() => authPage.evaluate(() => (window as any).__mqttStatusResolvers.length)).toBe(2);

      await authPage.evaluate(() => {
        (window as any).__mqttStatusResolvers[1]({
          success: true,
          data: {
            enabled: true,
            initialized: true,
            connected: true,
            networkConnected: true,
            server: 'new.example.com',
            port: 1883,
            clientId: 'new-client',
            reconnectCount: 1,
          },
        });
      });
      await expect(authPage.locator('#mqtt-status-badge')).toHaveText('已连接');
      await expect(authPage.locator('#mqtt-status-server')).toHaveText('new.example.com:1883');

      await authPage.evaluate(() => {
        (window as any).__mqttStatusResolvers[0]({
          success: true,
          data: {
            enabled: true,
            initialized: true,
            connected: false,
            stopped: true,
            autoReconnect: false,
            networkConnected: true,
            lastErrorCode: 4,
            lastErrorMessage: '旧请求错误，不应显示',
          },
        });
      });
      await authPage.waitForTimeout(100);
      await expect(authPage.locator('#mqtt-status-badge')).toHaveText('已连接');
      await expect(authPage.locator('#mqtt-status-server')).toHaveText('new.example.com:1883');
      await expect(authPage.locator('#mqtt-status-reason')).not.toContainText('旧请求错误');
    } finally {
      await authPage.evaluate(() => {
        const target = window as any;
        target.AppState._stopMqttStatusPolling();
        target.apiGetSilent = target.__mqttStatusOriginalGet;
        delete target.__mqttStatusOriginalGet;
        delete target.__mqttStatusResolvers;
      });
    }
  });
});
