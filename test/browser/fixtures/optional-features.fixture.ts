import { expect, type Page } from '@playwright/test';

/** Give feature-specific UI tests explicit prerequisites without changing device profiles. */
export async function enableOptionalUiFeatures(page: Page, keys: string[]) {
  for (const endpoint of ['system/capabilities', 'system/status', 'features/status']) {
    await page.route(`**/api/${endpoint}*`, async route => {
      const response = await route.fetch();
      const payload = await response.json();
      payload.data = payload.data || {};
      if (endpoint === 'system/capabilities') {
        keys.forEach(key => { payload.data[key] = true; });
        if (keys.includes('logViewer')) payload.data.logger = true;
      } else if (endpoint === 'system/status') {
        payload.data.features = payload.data.features || {};
        keys.forEach(key => { payload.data.features[key] = 'active'; });
      } else {
        const features = Array.isArray(payload.data.features) ? payload.data.features : [];
        keys.forEach(key => {
          const existing = features.find((feature: any) => feature.key === key);
          const state = { key, compiled: true, userEnabled: true, active: true, bootEnabled: true, budgetPassed: true };
          if (existing) Object.assign(existing, state);
          else features.push(state);
        });
        payload.data.features = features;
      }
      await route.fulfill({ response, json: payload });
    });
  }

  await page.evaluate(async () => {
    const w = window as any;
    await w.AppState._featureStatusPromise;
    w.apiInvalidateCache?.('/api/system/status');
    w.apiInvalidateCache?.('/api/features/status');
    await w.AppState.applyFeatureMenuVisibility();
  });
  for (const key of keys) {
    expect(await page.evaluate(feature => (window as any).AppState._featureActive?.[feature], key)).toBe(true);
  }
}
