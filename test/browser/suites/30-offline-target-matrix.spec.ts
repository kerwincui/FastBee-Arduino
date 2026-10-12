import { expect, test } from '../fixtures/base.fixture';
import { readFileSync } from 'node:fs';
import path from 'node:path';
import type { Page, TestInfo } from '@playwright/test';

type OfflineProfile = {
  chipModel: string;
  flashBytes: number;
  psramBytes: number;
  memoryProfile: string;
  enabledFeatures: string[];
  protocol: { mqttEnabled: boolean; modbusEnabled: boolean };
  evidence: string;
};

type Diagnostics = {
  consoleErrors: string[];
  pageErrors: string[];
  requestFailures: string[];
  httpErrors: string[];
  apiRequests: string[];
  stop: () => void;
};

const fixture = JSON.parse(readFileSync(
  path.resolve(__dirname, '..', 'offline-device-profiles.json'),
  'utf8'
)) as { profiles: Record<string, OfflineProfile> };

const pages = [
  { page: 'dashboard', root: 'dashboard-page', module: 'dashboard', title: '设备监控仪表盘' },
  { page: 'device', root: 'device-page', module: 'device', title: '设备配置' },
  { page: 'network', root: 'network-page', module: 'network', title: '网络设置' },
  { page: 'peripheral', root: 'peripheral-page', module: 'peripheral', title: '外设配置' },
  { page: 'periph-exec', root: 'periph-exec-page', module: 'peripheral', title: '外设执行' },
  { page: 'protocol', root: 'protocol-page', module: 'protocol', title: '通信协议' },
  { page: 'device-control', root: 'device-control-page', module: 'peripheral', title: '设备大屏', feature: 'deviceControl' },
  { page: 'screen-panel', root: 'screen-panel-page', module: 'screen-panel', title: '设备屏幕', feature: 'screenPanel' },
  { page: 'rule-script', root: 'rule-script-page', module: 'rule-script', title: '规则脚本', feature: 'ruleScript' },
  { page: 'logs', root: 'logs-page', module: 'logs', title: '设备日志', feature: 'logViewer' },
  { page: 'users', root: 'users-page', module: 'admin', title: '用户管理', feature: 'userAdmin' },
  { page: 'features', root: 'features-page', module: 'features', title: '功能配置' },
] as const;

const inactiveBusinessApiPrefixes: Record<string, string[]> = {
  'device-control': ['/api/periph-exec/controls', '/api/periph-exec/run', '/api/modbus/'],
  'screen-panel': ['/api/screen-panel/', '/api/periph-exec/history'],
  'rule-script': ['/api/rule-script'],
  logs: ['/api/logs'],
  users: ['/api/users'],
};

function profileFor(testInfo: TestInfo): { key: string; profile: OfflineProfile } {
  const key = String((testInfo.project.metadata as Record<string, unknown>).offlineProfile || '');
  const profile = fixture.profiles[key];
  expect(profile, `project ${testInfo.project.name} must declare a known offlineProfile`).toBeTruthy();
  return { key, profile };
}

function collectDiagnostics(page: Page, expectedOrigin?: string): Diagnostics {
  const consoleErrors: string[] = [];
  const pageErrors: string[] = [];
  const requestFailures: string[] = [];
  const httpErrors: string[] = [];
  const apiRequests: string[] = [];
  const origin = expectedOrigin || new URL(page.url()).origin;

  const onConsole = (message: import('@playwright/test').ConsoleMessage) => {
    if (message.type() === 'error') consoleErrors.push(message.text());
  };
  const onPageError = (error: Error) => pageErrors.push(error.message);
  const onRequestFailed = (request: import('@playwright/test').Request) => {
    if (new URL(request.url()).origin === origin) {
      requestFailures.push(`${request.method()} ${new URL(request.url()).pathname}: ${request.failure()?.errorText || 'failed'}`);
    }
  };
  const onResponse = (response: import('@playwright/test').Response) => {
    if (new URL(response.url()).origin === origin && response.status() >= 400) {
      httpErrors.push(`${response.status()} ${new URL(response.url()).pathname}`);
    }
  };
  const onRequest = (request: import('@playwright/test').Request) => {
    const url = new URL(request.url());
    if (url.origin === origin && url.pathname.startsWith('/api/')) {
      apiRequests.push(`${request.method()} ${url.pathname}`);
    }
  };

  page.on('console', onConsole);
  page.on('pageerror', onPageError);
  page.on('requestfailed', onRequestFailed);
  page.on('response', onResponse);
  page.on('request', onRequest);
  return {
    consoleErrors,
    pageErrors,
    requestFailures,
    httpErrors,
    apiRequests,
    stop: () => {
      page.off('console', onConsole);
      page.off('pageerror', onPageError);
      page.off('requestfailed', onRequestFailed);
      page.off('response', onResponse);
      page.off('request', onRequest);
    },
  };
}

async function waitForProfileReady(page: Page): Promise<void> {
  await page.waitForFunction(() => {
    const browserWindow = window as unknown as {
      AppState?: { _featureStatusReady?: boolean; _featureStatusPromise?: Promise<unknown> | null };
      apiGetGovernorState?: () => { inflight?: number; queueLength?: number };
    };
    const state = browserWindow.AppState;
    const governor = browserWindow.apiGetGovernorState?.();
    return state?._featureStatusReady === true && !state._featureStatusPromise &&
      (!governor || (governor.inflight === 0 && governor.queueLength === 0));
  }, undefined, { timeout: 20_000 });
  // Require one stable idle window so a just-scheduled dashboard batch cannot
  // be mistaken for a failure of the page being measured next.
  await page.waitForTimeout(100);
  await page.waitForFunction(() => {
    const getter = (window as unknown as {
      apiGetGovernorState?: () => { inflight?: number; queueLength?: number };
    }).apiGetGovernorState;
    const governor = getter?.();
    return !governor || (governor.inflight === 0 && governor.queueLength === 0);
  }, undefined, { timeout: 20_000 });
}

async function openProfileApp(page: Page): Promise<void> {
  await page.goto('/', { waitUntil: 'domcontentloaded', timeout: 30_000 });
  await page.waitForFunction(
    () => (window as unknown as { __fastbeeBootReady?: boolean }).__fastbeeBootReady === true,
    undefined,
    { timeout: 30_000 }
  );
  await page.locator('#app-container').waitFor({ state: 'visible', timeout: 30_000 });
  await waitForProfileReady(page);
}

async function changePage(page: Page, pageName: string, rootId: string, requireInteractive = true): Promise<void> {
  await page.evaluate(async (target) => {
    const state = (window as unknown as {
      AppState?: { changePage: (name: string) => Promise<void>; currentPage?: string };
    }).AppState;
    if (!state) throw new Error('AppState is not available');
    await state.changePage(target);
  }, pageName);
  await page.waitForFunction(([target, root, interactive]) => {
    const state = (window as unknown as { AppState?: { currentPage?: string } }).AppState;
    const element = document.getElementById(root);
    return state?.currentPage === target && !!element && element.classList.contains('active') &&
      (!interactive || (!element.hasAttribute('inert') && element.getAttribute('aria-busy') !== 'true'));
  }, [pageName, rootId, requireInteractive], { timeout: 20_000 });
}

async function expectStaticPageRoot(page: Page, moduleName: string, rootId: string): Promise<void> {
  const result = await page.evaluate(async ([module, root]) => {
    const response = await fetch(`/pages/${module}.html`, { cache: 'no-store' });
    const text = await response.text();
    const doc = new DOMParser().parseFromString(text, 'text/html');
    return { ok: response.ok, status: response.status, hasRoot: Boolean(doc.getElementById(root)) };
  }, [moduleName, rootId]);
  expect(result, `${moduleName}.html must publish #${rootId}`).toEqual({ ok: true, status: 200, hasRoot: true });
}

async function expectNoOverflow(page: Page, rootId: string): Promise<void> {
  const overflow = await page.evaluate((root) => {
    const element = document.getElementById(root);
    if (!element) return { missing: true };
    return {
      missing: false,
      viewportWidth: window.innerWidth,
      documentWidth: document.documentElement.scrollWidth,
      bodyWidth: document.body.scrollWidth,
      rootClientWidth: element.clientWidth,
      rootScrollWidth: element.scrollWidth,
      viewportOverflow: document.documentElement.scrollWidth > window.innerWidth + 1 ||
        document.body.scrollWidth > window.innerWidth + 1,
      rootOverflow: element.scrollWidth > element.clientWidth + 1,
    };
  }, rootId);
  expect(overflow, `#${rootId} must fit the configured viewport`).toMatchObject({
    missing: false,
    viewportOverflow: false,
    rootOverflow: false,
  });
}

async function expectCleanDiagnostics(diagnostics: Diagnostics, label: string): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, 250));
  diagnostics.stop();
  expect(diagnostics.consoleErrors, `${label} console errors`).toEqual([]);
  expect(diagnostics.pageErrors, `${label} page errors`).toEqual([]);
  expect(diagnostics.requestFailures, `${label} request failures`).toEqual([]);
  expect(diagnostics.httpErrors, `${label} HTTP errors`).toEqual([]);
}

test.describe('Offline target capability and runtime matrix', () => {
  for (const entry of pages) {
    test(`WEB-MATRIX-${entry.page}: visible or restricted state is explicit and responsive`, async ({ authPage }, testInfo) => {
      const { key, profile } = profileFor(testInfo);
      await waitForProfileReady(authPage);
      const diagnostics = collectDiagnostics(authPage);
      try {
        await expectStaticPageRoot(authPage, entry.module, entry.root);
        const menu = authPage.locator(`.menu-item[data-page="${entry.page}"]`);
        await expect(menu, `${key}: ${entry.page} menu is compiled`).toHaveCount(1);
        const runtimeActive = !entry.feature || profile.enabledFeatures.includes(entry.feature);

        if (runtimeActive) {
          await expect(menu, `${key}: ${entry.page} menu is visible only while runtime-active`).not.toHaveAttribute('aria-hidden', 'true');
          await expect(menu).not.toHaveCSS('display', 'none');
          await changePage(authPage, entry.page, entry.root);
          await expect(authPage.locator(`#${entry.root}`)).toBeVisible();
          await expect(authPage.locator('#page-title')).toHaveText(entry.title);
          await expectNoOverflow(authPage, entry.root);
        } else {
          await expect(menu, `${key}: inactive ${entry.feature} menu must fail closed`).toHaveAttribute('aria-hidden', 'true');
          await expect(menu).toHaveCSS('display', 'none');

          const apiRequestBaseline = diagnostics.apiRequests.length;
          await changePage(authPage, entry.page, entry.root);
          const fallback = authPage.locator(`#${entry.root} [data-page-fallback="feature-disabled"]`);
          await expect(fallback, `${key}: inactive ${entry.page} direct navigation explains the restriction`).toBeVisible();
          await expect(fallback).toContainText('该功能当前未启用');
          await expect(authPage.locator(`#${entry.root}`)).not.toHaveAttribute('inert', '');
          await expect(authPage.locator(`#${entry.root}`)).not.toHaveAttribute('aria-busy', 'true');
          await expect(authPage.locator(`#${entry.root}`)).not.toHaveAttribute('data-module-loading', 'true');
          await expectNoOverflow(authPage, entry.root);
          const businessPrefixes = inactiveBusinessApiPrefixes[entry.page] || [];
          const unexpectedBusinessRequests = diagnostics.apiRequests
            .slice(apiRequestBaseline)
            .filter((request) => businessPrefixes.some((prefix) => request.includes(` ${prefix}`)));
          expect(
            unexpectedBusinessRequests,
            `${key}: inactive ${entry.page} direct navigation must not start business APIs`
          ).toEqual([]);

          await changePage(authPage, 'features', 'features-page');
          const featureRow = authPage.locator(`#features-list tr[data-feature-key="${entry.feature}"]`);
          await expect(featureRow, `${key}: feature config explains why ${entry.page} is hidden`).toBeVisible();
          await expect(featureRow.locator('.feature-state')).toHaveText(/用户未启用|未编译|运行受限/);
          // Saved intent is not evidence of driver teardown or RAM release.
          await expect(featureRow.locator('.feature-state')).not.toHaveText(/已停用|已释放|运行中/);
          await expect(featureRow.locator('input[data-feature-toggle]')).not.toBeChecked();
          await expectNoOverflow(authPage, 'features-page');
        }

        await expectCleanDiagnostics(diagnostics, `${key}/${testInfo.project.name}/${entry.page}`);
      } finally {
        diagnostics.stop();
      }
    });
  }

  test('WEB-MATRIX-contract: compiled capabilities, runtime state and constrained controls stay distinct', async ({ page }, testInfo) => {
    const { key, profile } = profileFor(testInfo);
    const baseURL = String(testInfo.project.use.baseURL || '');
    const diagnostics = collectDiagnostics(page, new URL(baseURL).origin);
    try {
      await openProfileApp(page);
      const snapshot = await page.evaluate(async () => {
        const get = async (url: string) => (await (await fetch(url, { cache: 'no-store' })).json()).data;
        return {
          profile: await get('/api/test/offline-profile'),
          capabilities: await get('/api/system/capabilities'),
          features: await get('/api/features/status'),
          protocol: await get('/api/protocol/config'),
          webManifest: await (await fetch('/web-resource-manifest.json', { cache: 'no-store' })).json(),
        };
      });
      expect(snapshot.profile.environment).toBe(key);
      expect(snapshot.profile.memoryProfile).toBe(profile.memoryProfile);
      expect(snapshot.profile.enabledFeatures).toEqual(profile.enabledFeatures);
      expect(snapshot.protocol.mqtt.enabled).toBe(profile.protocol.mqttEnabled);
      expect(snapshot.protocol.modbusRtu.enabled).toBe(profile.protocol.modbusEnabled);

      const expectS3Network = key === 'esp32s3-F16R8';
      const expectOta = expectS3Network || key === 'esp32c6-F8R0';
      expect(snapshot.capabilities.ota).toBe(expectOta);
      expect(snapshot.capabilities.ethernet).toBe(expectS3Network);
      expect(snapshot.capabilities.cellular).toBe(expectS3Network);
      for (const capability of ['periphExec', 'mqtt', 'modbus', 'ruleScript', 'deviceControl', 'logViewer', 'userAdmin']) {
        expect(snapshot.capabilities[capability], `${key}: ${capability} is compiled, not necessarily active`).toBe(true);
      }

      const runtimeByKey = new Map(snapshot.features.features.map((feature: { key: string; active: boolean }) => [feature.key, feature.active]));
      for (const feature of ['ruleScript', 'deviceControl', 'logViewer', 'userAdmin']) {
        expect(runtimeByKey.get(feature), `${key}: ${feature} remains runtime-inactive unless deployment enables it`).toBe(false);
      }
      expect(runtimeByKey.get('modbus')).toBe(profile.protocol.modbusEnabled);

      await changePage(page, 'network', 'network-page');
      await expect(page.locator('#network-type option[value="1"]')).toHaveCount(expectS3Network ? 1 : 0);
      await expect(page.locator('#network-type option[value="2"]')).toHaveCount(expectS3Network ? 1 : 0);

      await changePage(page, 'device', 'device-page');
      const otaHint = page.locator('#dev-ota-capability-hint');
      const otaInput = page.locator('#dev-ota-file');
      if (expectOta) {
        await expect(otaInput).toBeEnabled();
        await expect(otaHint).toBeHidden();
      } else if (snapshot.webManifest.compiledCapabilities.ota === false) {
        // Target-specific resources remove the unsupported OTA tab and its controls.
        // A shared S3 asset tree retains the disabled runtime fallback below.
        await expect(page.locator('#device-page .config-tab[data-tab="dev-ota"]')).toHaveCount(0);
        await expect(page.locator('#dev-ota')).toHaveCount(0);
        await expect(otaInput).toHaveCount(0);
        await expect(page.locator('#dev-ota-upload-btn, #device-ota-container')).toHaveCount(0);
      } else {
        await expect(otaInput).toBeDisabled();
        await expect(otaHint).not.toHaveClass(/is-hidden/);
        await expect(otaHint).toContainText(/不支持.*OTA/);
      }

      await changePage(page, 'protocol', 'protocol-page');
      const modbusTab = page.locator('#protocol-page .config-tab[data-tab="modbus-rtu"]');
      await expect(modbusTab).toHaveAttribute('data-feature-state', profile.protocol.modbusEnabled ? 'active' : 'disabled');
      if (profile.protocol.modbusEnabled) await expect(modbusTab).toBeVisible();
      else await expect(modbusTab).toBeHidden();
      await expectNoOverflow(page, 'protocol-page');

      await expectCleanDiagnostics(diagnostics, `${key}/${testInfo.project.name}/contract`);
    } finally {
      diagnostics.stop();
    }
  });
});
