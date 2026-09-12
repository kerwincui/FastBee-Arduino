import { test, expect } from '../fixtures/base.fixture';

/**
 * Suite-21: 静态资源加载失败重试（弱网/退化链路防护）
 *
 * 背景：WiFi 链路退化（高延迟/大量重传）时，大包静态资源（CSS/JS chunk）
 * 单次加载失败率高。index.html 的 chunk 加载器与 main.js 的 CSS 加载器
 * 必须失败重试（而非跳过），否则登录页白屏。
 *
 * 通过 route 拦截模拟首次加载失败，验证重试后页面恢复。
 */
test.describe('Suite-21: 资源加载失败重试', () => {

  test('RETRY-001: chunk 首次加载失败后自动重试成功 @quick', async ({ page }) => {
    let abortCount = 0;
    let requestCount = 0;

    // 拦截 chunk-2 首次请求并中断（模拟弱网丢包），后续放行
    await page.route('**/js/chunk-2-app.js*', async (route) => {
      requestCount++;
      if (abortCount === 0) {
        abortCount++;
        await route.abort('failed');
      } else {
        await route.continue();
      }
    });

    await page.goto('/');
    // 重试退避 500ms 起步，等待重试完成与后续 chunk 串行加载
    await expect(page.locator('#login-page')).toBeVisible({ timeout: 20_000 });

    // 必须有至少 2 次请求（1 次失败 + 1 次重试成功）
    await expect.poll(() => requestCount, { timeout: 20_000 }).toBeGreaterThanOrEqual(2);
    // 登录表单可用 = 页面骨架完整渲染（未被失败 chunk 卡死）
    await expect(page.locator('#username')).toBeVisible();
    await expect(page.locator('#login-button')).toBeVisible();
  });

  test('RETRY-002: 多个 chunk 连续失败仍能通过重试恢复', async ({ page }) => {
    const failOnce = new Set<string>();

    // 每个 chunk 的首次请求都失败一次，重试时放行
    await page.route('**/js/chunk-*.js*', async (route) => {
      const url = route.request().url();
      const key = url.split('?')[0];
      if (!failOnce.has(key)) {
        failOnce.add(key);
        await route.abort('connectionfailed');
      } else {
        await route.continue();
      }
    });

    await page.goto('/');
    await expect(page.locator('#login-page')).toBeVisible({ timeout: 30_000 });
    await expect(page.locator('#login-button')).toBeVisible({ timeout: 30_000 });
  });

  test('RETRY-003: CSS 加载失败不影响登录页骨架渲染', async ({ page }) => {
    let cssRequests = 0;
    // CSS 首次失败，重试放行；验证页面不会因 CSS 失败而白屏/卡死
    await page.route('**/css/main.css*', async (route) => {
      cssRequests++;
      if (cssRequests === 1) {
        await route.abort('failed');
      } else {
        await route.continue();
      }
    });

    await page.goto('/');
    await expect(page.locator('#login-page')).toBeVisible({ timeout: 20_000 });
    // 登录表单始终可交互（CSS 失败不阻塞功能）
    await page.fill('#username', 'admin');
    await expect(page.locator('#username')).toHaveValue('admin');
    // CSS 发生重试（至少 2 次请求）
    await expect.poll(() => cssRequests, { timeout: 20_000 }).toBeGreaterThanOrEqual(2);
  });

  test('RETRY-004: 正常网络下全部资源一次加载成功（基线）', async ({ page }) => {
    const failed: string[] = [];
    page.on('requestfailed', (req) => {
      const url = req.url();
      if (url.includes('.js') || url.includes('.css')) failed.push(url);
    });

    await page.goto('/');
    await expect(page.locator('#login-page')).toBeVisible({ timeout: 20_000 });
    await expect(page.locator('#login-button')).toBeVisible();
    // 基线场景不应有资源最终失败（首次失败可被重试掩盖，此处检查控制台无 give-up）
    const gaveUp = await page.evaluate(() =>
      (window as unknown as { __fastbeeChunkGaveUp?: boolean }).__fastbeeChunkGaveUp === true
    );
    expect(gaveUp).toBeFalsy();
  });
});
