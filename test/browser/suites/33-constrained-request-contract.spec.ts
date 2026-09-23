import { expect, test } from '@playwright/test';
import { readFileSync } from 'node:fs';
import path from 'node:path';

const sourceRoot = path.resolve(__dirname, '..', '..', '..', 'web-src', 'modules', 'runtime');

async function useRuntimeSource(page: import('@playwright/test').Page, moduleName: string): Promise<void> {
  const body = readFileSync(path.join(sourceRoot, `${moduleName}.js`), 'utf8');
  await page.route(`**/js/modules/${moduleName}.js*`, async (route) => {
    await route.fulfill({ status: 200, contentType: 'text/javascript; charset=utf-8', body });
  });
}

async function openApp(page: import('@playwright/test').Page): Promise<void> {
  await page.goto('/', { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(
    () => (window as unknown as { __fastbeeBootReady?: boolean }).__fastbeeBootReady === true
  );
  await page.locator('#app-container').waitFor({ state: 'visible' });
  await page.waitForFunction(() => {
    const state = (window as unknown as { AppState?: { _featureStatusReady?: boolean } }).AppState;
    return state?._featureStatusReady === true;
  });
}

async function changePage(
  page: import('@playwright/test').Page,
  pageName: string,
  rootId: string
): Promise<void> {
  await page.evaluate(async (target) => {
    const state = (window as unknown as {
      AppState: { changePage: (name: string) => Promise<void> };
    }).AppState;
    await state.changePage(target);
  }, pageName);
  await page.locator(`#${rootId}.active`).waitFor({ state: 'visible' });
  await page.waitForFunction(() => {
    const getter = (window as unknown as {
      apiGetGovernorState?: () => { inflight?: number; queueLength?: number };
    }).apiGetGovernorState;
    const state = getter?.();
    return !state || (state.inflight === 0 && state.queueLength === 0);
  });
}

function apiPath(url: string): string {
  const parsed = new URL(url);
  return `${parsed.pathname}${parsed.search}`;
}

function logicalApiPaths(request: import('@playwright/test').Request): string[] {
  const outer = apiPath(request.url());
  if (!outer.startsWith('/api/batch')) return [outer];
  try {
    const payload = request.postDataJSON() as { requests?: Array<{ url?: string }> };
    return (payload.requests || [])
      .map((entry) => String(entry.url || ''))
      .filter((url) => url.startsWith('/api/'));
  } catch {
    return [];
  }
}

test('WEB-CONSTRAINED-REQUEST-001: peripheral type catalog loads only when the editor opens', async ({ page }) => {
  await useRuntimeSource(page, 'peripherals');
  const requests: string[] = [];
  page.on('request', (request) => {
    requests.push(...logicalApiPaths(request).filter((url) => url.startsWith('/api/peripherals')));
  });

  await openApp(page);
  const baseline = requests.length;
  await changePage(page, 'peripheral', 'peripheral-page');
  await expect.poll(() => requests.slice(baseline).some(
    (request) => request === '/api/peripherals' || request.startsWith('/api/peripherals?')
  )).toBeTruthy();
  const firstScreen = requests.slice(baseline);

  expect(firstScreen.some((request) =>
    request === '/api/peripherals' || request.startsWith('/api/peripherals?')
  )).toBeTruthy();
  expect(firstScreen.some((request) => request.startsWith('/api/peripherals/types'))).toBeFalsy();

  await page.locator('#add-peripheral-btn').click();
  await page.locator('#peripheral-modal').waitFor({ state: 'visible' });
  await expect.poll(
    () => requests.filter((request) => request.startsWith('/api/peripherals/types')).length
  ).toBe(1);
});

test('WEB-CONSTRAINED-REQUEST-002: execution filter options load once on first interaction', async ({ page }) => {
  await useRuntimeSource(page, 'periph-exec');
  const requests: string[] = [];
  let filterRequests = 0;
  await page.route(/\/api\/peripherals(?:\?|$)/, async (route) => {
    filterRequests += 1;
    await new Promise((resolve) => setTimeout(resolve, 150));
    await route.continue();
  });
  await page.route('**/api/batch', async (route) => {
    const paths = logicalApiPaths(route.request());
    const count = paths.filter((url) => url.startsWith('/api/peripherals?')).length;
    filterRequests += count;
    if (count > 0) await new Promise((resolve) => setTimeout(resolve, 150));
    await route.continue();
  });
  page.on('request', (request) => {
    requests.push(...logicalApiPaths(request).filter((url) =>
      url.startsWith('/api/periph-exec') || url.startsWith('/api/peripherals')
    ));
  });

  await openApp(page);
  const baseline = requests.length;
  await changePage(page, 'periph-exec', 'periph-exec-page');
  await expect.poll(() => requests.slice(baseline).some(
    (request) => request === '/api/periph-exec' || request.startsWith('/api/periph-exec?')
  )).toBeTruthy();
  const firstScreen = requests.slice(baseline);

  expect(firstScreen.some((request) =>
    request === '/api/periph-exec' || request.startsWith('/api/periph-exec?')
  )).toBeTruthy();
  expect(firstScreen.some((request) => request.startsWith('/api/peripherals?'))).toBeFalsy();

  const filter = page.locator('#periph-exec-filter-periph');
  const initialOptionCount = await filter.locator('option').count();
  await filter.dispatchEvent('pointerdown');
  await filter.focus();
  await expect.poll(() => filterRequests).toBe(1);
  await expect.poll(() => filter.locator('option').count()).toBeGreaterThan(initialOptionCount);
  expect(filterRequests).toBe(1);
});
