import {
  test,
  expect,
  env,
  buildSafeHttpFailureDiagnostic,
  captureHttpFailureDiagnostic,
} from '../fixtures/base.fixture';

// Filesystem-only firmware updates must be tested against the current device
// response, not an older Service Worker cache.
test.use({ serviceWorkers: 'block' });

type RootLoadResult = {
  status: number;
  elapsedMs: number;
  staticFailures: string[];
  pageErrors: string[];
};

const STATIC_RESOURCE_TYPES = new Set(['document', 'script', 'stylesheet', 'font']);

function durationLimitFromEnv(name: string, fallbackMs: number): number {
  const parsed = Number(process.env[name]);
  if (!Number.isFinite(parsed) || parsed <= 0) return fallbackMs;
  return Math.max(3000, Math.min(60_000, Math.round(parsed)));
}

// A 13-15s response is an operator-visible stall, not a successful C3 load.
// Environments with intentionally slow links can override these gates without
// weakening the default acceptance criteria used by deployment verification.
const ROOT_LOAD_MAX_MS = durationLimitFromEnv('WEB_ROOT_LOAD_MAX_MS', 10_000);
const REFRESH_LOAD_MAX_MS = durationLimitFromEnv('WEB_REFRESH_LOAD_MAX_MS', 10_000);
const ROOT_P95_MAX_MS = durationLimitFromEnv('WEB_ROOT_P95_MAX_MS', 8_000);

function percentile95(values: number[]): number {
  if (values.length === 0) return 0;
  const sorted = [...values].sort((left, right) => left - right);
  return sorted[Math.min(sorted.length - 1, Math.max(0, Math.ceil(sorted.length * 0.95) - 1))];
}

/**
 * Load the real device root page and wait until the application boot marker is
 * published. A transient 503 is retried because the firmware intentionally
 * sheds work while a previous static response is draining.
 */
async function loadRootPage(
  page: import('@playwright/test').Page,
  testInfo: import('@playwright/test').TestInfo,
  loadLabel: string
): Promise<RootLoadResult> {
  let lastError: unknown = null;
  const maxAttempts = 3;
  let retryDelayMs = 1500;

  for (let attempt = 1; attempt <= maxAttempts; attempt++) {
    // A previous document may still have a late stylesheet/font response in
    // flight when the embedded server releases the root response. Stop that
    // old document before attaching listeners for the next navigation; its
    // browser-generated ERR_ABORTED must not be counted as a device resource
    // failure for the new page.
    if (attempt === 1) {
      await page.evaluate(() => window.stop()).catch(() => {});
    }
    // A constrained target can reject an early resource attempt with 503 and
    // serve the browser retry successfully. Track the terminal outcome per
    // resource instead of treating a recovered attempt as permanent damage.
    const staticFailuresByResource = new Map<string, string[]>();
    const pageErrors: string[] = [];
    const staticResourceKey = (url: string) => {
      try {
        const parsed = new URL(url);
        return `${parsed.origin}${parsed.pathname}`;
      } catch {
        return url.split('?')[0];
      }
    };
    const addStaticFailure = (url: string, message: string) => {
      const key = staticResourceKey(url);
      const failures = staticFailuresByResource.get(key) || [];
      failures.push(message);
      staticFailuresByResource.set(key, failures);
    };
    const onRequestFailed = (request: import('@playwright/test').Request) => {
      if (STATIC_RESOURCE_TYPES.has(request.resourceType())) {
        addStaticFailure(
          request.url(),
          `${request.resourceType()} ${request.url()} (${request.failure()?.errorText || 'failed'})`
        );
      }
    };
    const onResponse = (response: import('@playwright/test').Response) => {
      const request = response.request();
      if (!STATIC_RESOURCE_TYPES.has(request.resourceType())) return;
      if (response.status() >= 400) {
        addStaticFailure(request.url(), `${request.resourceType()} ${request.url()} (HTTP ${response.status()})`);
      } else {
        staticFailuresByResource.delete(staticResourceKey(request.url()));
      }
    };
    const onPageError = (error: Error) => pageErrors.push(error.message);

    page.on('requestfailed', onRequestFailed);
    page.on('response', onResponse);
    page.on('pageerror', onPageError);
    const startedAt = Date.now();

    try {
      // ESPAsyncWebServer may keep the document response open while static
      // resources are being served. `commit` gives us the HTTP response as
      // soon as the document is available; the explicit DOM/boot waits below
      // retain the readiness checks without treating that open connection as
      // a page-load failure.
      const response = await page.goto('/', { waitUntil: 'commit', timeout: 30_000 });
      if (!response) throw new Error('Root navigation returned no response');
      const status = response.status();
      if (status >= 500) {
        const diagnostic = await captureHttpFailureDiagnostic(
          response,
          testInfo,
          `${loadLabel}-attempt-${attempt}`
        );
        const retryAfterSeconds = Number(diagnostic.retryAfter);
        retryDelayMs = Number.isFinite(retryAfterSeconds) && retryAfterSeconds > 0
          ? Math.max(1500, retryAfterSeconds * 1000)
          : 1500 * attempt;
        throw new Error(
          `Root navigation returned HTTP ${status}; ` +
          `Retry-After=${diagnostic.retryAfter || 'not-provided'}; ` +
          `server reason=${diagnostic.serverReason || 'not-provided'}`
        );
      }

      await page.waitForFunction(
        () => document.readyState !== 'loading',
        undefined,
        { timeout: 15_000 }
      );

      await page.waitForFunction(
        () => (window as unknown as { __fastbeeBootReady?: boolean }).__fastbeeBootReady === true,
        undefined,
        { timeout: 20_000 }
      );
      await page.waitForFunction(() => {
        const login = document.querySelector('#login-page');
        const app = document.querySelector('#app-container');
        const visible = (element: Element | null) => {
          if (!element) return false;
          const style = window.getComputedStyle(element);
          return style.display !== 'none' && style.visibility !== 'hidden';
        };
        return visible(login) || visible(app);
      }, undefined, { timeout: 10_000 });

      // Let late dynamic script/style retries settle before evaluating the
      // resource-failure budget.
      await page.waitForTimeout(500);
      page.off('requestfailed', onRequestFailed);
      page.off('response', onResponse);
      page.off('pageerror', onPageError);
      return {
        status,
        elapsedMs: Date.now() - startedAt,
        staticFailures: Array.from(staticFailuresByResource.values()).flat(),
        pageErrors,
      };
    } catch (error) {
      lastError = error;
      page.off('requestfailed', onRequestFailed);
      page.off('response', onResponse);
      page.off('pageerror', onPageError);
      if (attempt < maxAttempts) await page.waitForTimeout(retryDelayMs);
    }
  }

  throw lastError instanceof Error ? lastError : new Error(String(lastError));
}

test.describe('Suite-24: Web 页面流畅访问', () => {
  test('WEB-001: 首页启动和关键静态资源完整加载 @quick', async ({ page }, testInfo) => {
    const result = await loadRootPage(page, testInfo, 'web-root-single');
    expect(result.status, `首页 HTTP 状态异常: ${result.status}`).toBe(200);
    expect(result.staticFailures, `静态资源加载失败:\n${result.staticFailures.join('\n')}`).toEqual([]);
    expect(result.pageErrors, `页面启动异常:\n${result.pageErrors.join('\n')}`).toEqual([]);
    expect(
      result.elapsedMs,
      `首页加载过慢: ${result.elapsedMs}ms，门槛 ${ROOT_LOAD_MAX_MS}ms`
    ).toBeLessThanOrEqual(ROOT_LOAD_MAX_MS);
  });

  test('WEB-002: 连续刷新后页面仍可用 @quick', async ({ page }, testInfo) => {
    test.setTimeout(120_000);
    const rounds = Math.max(2, Math.min(4, Number(process.env.WEB_REFRESH_ROUNDS || 3)));
    const delayMs = Math.max(1000, Math.min(8000, Number(process.env.WEB_REFRESH_DELAY_MS || 2500)));
    const timings: number[] = [];

    for (let round = 0; round < rounds; round++) {
      const result = await loadRootPage(page, testInfo, `web-root-refresh-${round + 1}`);
      expect(result.status, `第 ${round + 1} 次首页 HTTP 状态异常`).toBe(200);
      expect(result.staticFailures, `第 ${round + 1} 次静态资源失败:\n${result.staticFailures.join('\n')}`).toEqual([]);
      expect(result.pageErrors, `第 ${round + 1} 次页面异常:\n${result.pageErrors.join('\n')}`).toEqual([]);
      expect(
        result.elapsedMs,
        `第 ${round + 1} 次首页加载过慢: ${result.elapsedMs}ms，门槛 ${REFRESH_LOAD_MAX_MS}ms`
      ).toBeLessThanOrEqual(REFRESH_LOAD_MAX_MS);
      timings.push(result.elapsedMs);
      if (round + 1 < rounds) await page.waitForTimeout(delayMs);
    }

    const p95 = percentile95(timings);
    console.log(
      `[WEB] ${env.deviceIp} root-load-ms=${timings.join(',')} ` +
      `p95=${p95} limits(single=${ROOT_LOAD_MAX_MS},refresh=${REFRESH_LOAD_MAX_MS},p95=${ROOT_P95_MAX_MS})`
    );
    expect(p95, `连续刷新首页 P95 ${p95}ms 超过门槛 ${ROOT_P95_MAX_MS}ms`)
      .toBeLessThanOrEqual(ROOT_P95_MAX_MS);
  });

  test('WEB-004: HTTP 失败诊断保留原因并脱敏 @quick', async () => {
    const diagnostic = buildSafeHttpFailureDiagnostic({
      label: 'contract-check',
      method: 'POST',
      path: '/api/auth/login',
      status: 503,
      statusText: 'Service Unavailable',
      retryAfter: '5',
      responseBody: JSON.stringify({
        message: 'Server low memory',
        password: 'do-not-persist-password',
        nested: { accessToken: 'do-not-persist-token' },
      }),
    });

    expect(diagnostic.retryAfter).toBe('5');
    expect(diagnostic.serverReason).toBe('Server low memory');
    expect(diagnostic.responseBody).toContain('[REDACTED]');
    expect(diagnostic.responseBody).not.toContain('do-not-persist-password');
    expect(diagnostic.responseBody).not.toContain('do-not-persist-token');

    const plainTextDiagnostic = buildSafeHttpFailureDiagnostic({
      label: 'plain-text-contract-check',
      method: 'GET',
      path: '/',
      status: 503,
      statusText: 'Service Unavailable',
      retryAfter: '5',
      reasonHeader: 'Static busy; token=do-not-persist-header-token',
      responseBody: 'Low memory; password=do-not-persist-plain-password',
    });
    expect(plainTextDiagnostic.serverReason).not.toContain('do-not-persist-header-token');
    expect(plainTextDiagnostic.responseBody).not.toContain('do-not-persist-plain-password');
  });

  test('WEB-003: 独立大屏脚本完整且数据完成刷新 @quick', async ({ authPage: mainPage }, testInfo) => {
    test.setTimeout(90_000);
    // The standalone monitor has its own document. Keep the authenticated app
    // alive so stopping its pending boot requests cannot pollute this check.
    const authPage = await mainPage.context().newPage();
    const staticFailuresByResource = new Map<string, string[]>();
    const requestFailures: string[] = [];
    const apiFailures: string[] = [];
    const pageErrors: string[] = [];
    const consoleErrors: string[] = [];
    const staticResourceKey = (url: string) => {
      try {
        const parsed = new URL(url);
        return `${parsed.origin}${parsed.pathname}`;
      } catch {
        return url.split('?')[0];
      }
    };
    const addStaticFailure = (url: string, message: string) => {
      const key = staticResourceKey(url);
      const failures = staticFailuresByResource.get(key) || [];
      failures.push(message);
      staticFailuresByResource.set(key, failures);
    };
    const onRequestFailed = (request: import('@playwright/test').Request) => {
      if (STATIC_RESOURCE_TYPES.has(request.resourceType())) {
        addStaticFailure(
          request.url(),
          `${request.resourceType()} ${request.url()} (${request.failure()?.errorText || 'failed'})`
        );
      } else {
        requestFailures.push(`${request.method()} ${request.url()} (${request.failure()?.errorText || 'failed'})`);
      }
    };
    const onResponse = (response: import('@playwright/test').Response) => {
      const request = response.request();
      if (STATIC_RESOURCE_TYPES.has(request.resourceType())) {
        if (response.status() >= 400) {
          addStaticFailure(request.url(), `${request.resourceType()} ${request.url()} (HTTP ${response.status()})`);
        } else {
          staticFailuresByResource.delete(staticResourceKey(request.url()));
        }
      } else if (response.url().includes('/api/') && response.status() >= 400) {
        apiFailures.push(`${request.method()} ${response.url()} (HTTP ${response.status()})`);
      }
    };
    const onPageError = (error: Error) => pageErrors.push(error.message);
    const onConsole = (message: import('@playwright/test').ConsoleMessage) => {
      if (message.type() === 'error' || message.type() === 'warning') {
        consoleErrors.push(`[${message.type()}] ${message.text()}`);
      }
    };

    authPage.on('requestfailed', onRequestFailed);
    authPage.on('response', onResponse);
    authPage.on('pageerror', onPageError);
    authPage.on('console', onConsole);
    try {
      let responseStatus = 0;
      let retryDelayMs = 1500;
      for (let attempt = 1; attempt <= 3; attempt++) {
        const response = await authPage.goto('/pages/fullscreen.html', { waitUntil: 'commit', timeout: 30_000 });
        if (!response) throw new Error('大屏页面导航没有返回 HTTP 响应');
        responseStatus = response.status();
        if (responseStatus !== 503) break;

        const diagnostic = await captureHttpFailureDiagnostic(
          response,
          testInfo,
          `fullscreen-attempt-${attempt}`
        );
        if (attempt === 3) {
          throw new Error(
            `大屏页面连续返回 HTTP 503；Retry-After=${diagnostic.retryAfter || 'not-provided'}；` +
            `server reason=${diagnostic.serverReason || 'not-provided'}`
          );
        }
        const retryAfterSeconds = Number(diagnostic.retryAfter);
        retryDelayMs = Number.isFinite(retryAfterSeconds) && retryAfterSeconds > 0
          ? Math.max(1500, retryAfterSeconds * 1000)
          : 1500 * attempt;
        await authPage.waitForTimeout(retryDelayMs);
      }
      expect(responseStatus, '大屏页面 HTTP 状态异常').toBe(200);
      await authPage.waitForFunction(
        () => document.readyState !== 'loading',
        undefined,
        { timeout: 15_000 }
      );
      await expect(authPage.locator('#auto-refresh-indicator')).toBeVisible({ timeout: 10_000 });
      try {
        await expect.poll(async () => {
          return (await authPage.locator('#monitor-uptime').textContent())?.trim() || '';
        }, { timeout: 15_000, message: '大屏运行数据应完成首次刷新' }).not.toBe('--');
      } catch (error) {
        const diagnostic = [
          error instanceof Error ? error.message : String(error),
          ...apiFailures,
          ...requestFailures,
          ...consoleErrors,
        ].filter(Boolean).join('\n');
        throw new Error(diagnostic);
      }
      await authPage.waitForTimeout(500);
    } finally {
      authPage.off('requestfailed', onRequestFailed);
      authPage.off('response', onResponse);
      authPage.off('pageerror', onPageError);
      authPage.off('console', onConsole);
      await authPage.close();
    }

    const staticFailures = Array.from(staticFailuresByResource.values()).flat();
    expect(staticFailures, `大屏静态资源加载失败:\n${staticFailures.join('\n')}`).toEqual([]);
    expect(apiFailures, `大屏 API 请求失败:\n${apiFailures.join('\n')}`).toEqual([]);
    expect(pageErrors, `大屏页面异常:\n${pageErrors.join('\n')}`).toEqual([]);
    expect(consoleErrors, `大屏控制台异常:\n${consoleErrors.join('\n')}`).toEqual([]);
  });
});
