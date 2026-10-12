import { test, expect } from '../fixtures/base.fixture';
import { enableOptionalUiFeatures } from '../fixtures/optional-features.fixture';
const { MAPPING: privateNames } = require('../../../scripts/web-private-properties');

// This suite loads published gzip assets through PageLoader/ModuleLoader and
// the offline API. Suite 28 separately exercises source-level failure cases.
test.describe('Generic peripheral published Web integration', () => {
  test('WEB-GENERIC-004: screen panel lazy modules preserve the saved rectangular layout', async ({ authPage, navigateTo }, info) => {
    const errors: string[] = [];
    const modules: string[] = [];
    authPage.on('pageerror', error => errors.push(error.message));
    authPage.on('request', request => {
      const pathname = new URL(request.url()).pathname;
      if (/\/js\/modules\/screen-panel(?:-model)?\.js$/.test(pathname)) modules.push(pathname);
    });
    await enableOptionalUiFeatures(authPage, ['screenPanel']);
    await authPage.route('**/api/screen-panel/*', async route => {
      const pathname = new URL(route.request().url()).pathname;
      expect(route.request().method()).toBe('GET');
      const data = pathname.endsWith('/config') ? {
        revision: 1, application: { status: 'running' }, capabilities: {},
        config: { schemaVersion: 1, displayRef: null,
          settings: { homePageId: 'home', background: { presetId: 'solid' } },
          pages: [{ id: 'home', title: '横屏', layoutId: 'mixed-grid',
            logicalWidth: 320, logicalHeight: 240, cards: [] }] },
      } : pathname.endsWith('/choices') ? { items: [], total: 0 }
        : { items: {}, application: { status: 'running' } };
      await route.fulfill({ json: { success: true, data } });
    });
    expect(modules).toEqual([]);
    await navigateTo('screen-panel');
    await expect(authPage.locator('#sp-runtime-badge')).toHaveText('运行中');
    await expect(authPage.locator('#sp-canvas-size')).toHaveText('逻辑尺寸 320 × 240');
    await expect(authPage.locator('.page-feature-fallback')).toHaveCount(0);
    expect(modules).toEqual(['/js/modules/screen-panel-model.js', '/js/modules/screen-panel.js']);
    await expect(authPage.locator('#sp-config')).toBeVisible();
    await expect(authPage.locator('#sp-save-config')).toBeVisible();
    await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-output-width', '320');
    await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-output-height', '240');
    for (const width of [1280, 390]) {
      await authPage.setViewportSize({ width, height: 844 });
      await expect(authPage.locator('#sp-editor-frame')).toBeVisible();
      const aspect = await authPage.locator('#sp-editor-canvas').evaluate(node => {
        const rect = node.getBoundingClientRect(); return rect.width / rect.height;
      });
      expect(aspect).toBeCloseTo(320 / 240, 1);
      await expect(authPage.locator('#sp-editor-canvas')).toBeVisible();
      await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-output-width', '320');
      await expect(authPage.locator('#sp-editor-canvas')).toHaveAttribute('data-output-height', '240');
      await expect(authPage.locator('#sp-save-config')).toBeVisible();
      await authPage.screenshot({ path: info.outputPath(`published-panel-${width}.png`) });
    }
    await navigateTo('dashboard');
    await navigateTo('screen-panel');
    expect(modules).toHaveLength(2);
    expect(errors).toEqual([]);
  });

  test('WEB-GENERIC-003: pin validation waits for the lazy save module and sends one request', async ({ authPage, navigateTo }) => {
    const errors: string[] = [];
    authPage.on('pageerror', error => errors.push(error.message));
    await navigateTo('peripheral');
    // Modal templates share the serialized static asset queue with modules.
    // Cache them before holding the save script so the test can open the form.
    await authPage.evaluate(() => (window as any).AppState._loadModals());
    let releaseSaveModule: () => void = () => {};
    const saveModuleReady = new Promise<void>(resolve => { releaseSaveModule = resolve; });
    let moduleRequests = 0;
    let validationRequests = 0;
    await authPage.route('**/js/modules/peripheral-save.js*', async route => {
      moduleRequests++;
      await saveModuleReady;
      await route.continue();
    });
    await authPage.route('**/api/peripherals/validate-pins*', async route => {
      validationRequests++;
      expect(new URL(route.request().url()).searchParams.get('pins')).toBe('4');
      await route.fulfill({ json: { success: true, allValid: false, pins: [{ pin: 4, error: '引脚冲突测试' }] } });
    });
    try {
      await authPage.locator('#add-peripheral-btn').click();
      const modal = authPage.locator('#peripheral-modal');
      await expect(modal).toBeVisible();
      const typeSelect = modal.locator('#peripheral-type-input');
      await expect(typeSelect).toBeEnabled();
      await expect(typeSelect.locator('option[value="12"]')).toHaveCount(1);
      await typeSelect.selectOption('12');
      await modal.locator('#peripheral-pins-input').fill('4');
      await modal.locator('#peripheral-name-input').focus();
      expect(validationRequests).toBe(0);
      releaseSaveModule();
      await expect(modal.locator('#peripheral-pins-validation-hint')).toHaveText('GPIO4: 引脚冲突测试');
      expect(moduleRequests).toBe(1);
      expect(validationRequests).toBe(1);
      expect(errors).toEqual([]);
    } finally {
      releaseSaveModule();
    }
  });

  test('WEB-GENERIC-001: INA226 configuration survives modal destruction at desktop and mobile', async ({ authPage, navigateTo }, info) => {
    const errors: string[] = [];
    authPage.on('pageerror', error => errors.push(error.message));
    await navigateTo('peripheral');
    for (const width of [1280, 390]) {
      await authPage.setViewportSize({ width, height: 844 });
      await authPage.locator('#add-peripheral-btn').click();
      const modal = authPage.locator('#peripheral-modal');
      await expect(modal).toBeVisible();
      const typeSelect = modal.locator('#peripheral-type-input');
      await expect(typeSelect).toBeEnabled();
      await expect(typeSelect.locator('option[value="38"]')).toHaveCount(1);
      await typeSelect.selectOption('38');
      await modal.locator('#peripheral-id-input').fill(`ui_ina_${width}`);
      await modal.locator('#peripheral-name-input').fill(`INA226 ${width}`);
      await modal.locator('#generic-sensor-driver').selectOption('ina226');
      await expect(modal.locator('#generic-sensor-driver option')).toHaveCount(8);
      await modal.locator('#generic-sensor-bus-id').selectOption('offline_i2c');
      await modal.locator('#generic-sensor-channel').selectOption('power');
      await modal.locator('#generic-driver-shuntResistance').fill('0.02');
      await modal.locator('#generic-driver-maxCurrent').fill('3');
      await modal.locator('#generic-driver-averaging').selectOption('4');
      expect(await modal.locator('.modal-content').evaluate(el => el.scrollWidth <= el.clientWidth + 1)).toBe(true);
      await authPage.screenshot({ path: info.outputPath(`published-ina-${width}.png`) });
      await modal.locator('#save-peripheral-btn').click();
      await expect(modal).toHaveCount(0);
      await authPage.evaluate(id => (window as any).AppState.openPeripheralModal(true, id), `ui_ina_${width}`);
      await expect(modal.locator('#generic-sensor-driver')).toHaveValue('ina226');
      await expect(modal.locator('#generic-driver-shuntResistance')).toHaveValue('0.02');
      await expect(modal.locator('#generic-driver-maxCurrent')).toHaveValue('3');
      await expect(modal.locator('#generic-driver-averaging')).toHaveValue('4');
      await expect(modal.locator('#generic-sensor-channel')).toHaveValue('voltage');
      await expect(modal.locator('#generic-sensor-bus-id')).toHaveValue('offline_i2c');
      await modal.locator('#cancel-peripheral-btn').click();
      await expect(modal).toHaveCount(0);
    }
    expect(errors).toEqual([]);
  });

  test('WEB-GENERIC-002: execution editor loads configured sensor metadata without visiting peripherals', async ({ authPage, navigateTo }, info) => {
    const errors: string[] = [];
    authPage.on('pageerror', error => errors.push(error.message));
    await navigateTo('periph-exec');
    await authPage.locator('#periph-exec-page-add-btn').click();
    const modal = authPage.locator('#periph-exec-modal');
    await expect(modal).toBeVisible();
    const block = modal.locator('#periph-exec-actions .periph-exec-config-item').first();
    await expect(block).toBeVisible();
    await block.locator('.pe-action-type').selectOption('19');
    await expect(block.locator('.pe-sensor-category')).toBeVisible();
    await block.locator('.pe-sensor-category').selectOption('configured');
    await block.locator('.pe-target-periph').selectOption('offline_ina');
    await expect(block.locator('.pe-sensor-channel-field option')).toHaveCount(3);
    await block.locator('.pe-sensor-channel-field').selectOption('current');
    await block.locator('.pe-sensor-property').fill('loadCurrent');
    await expect(block.locator('.pe-sensor-unit')).toHaveValue('A');
    const payload = await authPage.evaluate(names => {
      const app = (window as any).AppState;
      const key = names._collectPeriphExecActions in app ? names._collectPeriphExecActions : '_collectPeriphExecActions';
      return JSON.parse(app[key]()[0].actionValue);
    }, privateNames);
    expect(payload).toMatchObject({ periphId: 'offline_ina', sensorCategory: 'INA226', channelField: 'current', dataField: 'loadCurrent', unit: 'A', scaleFactor: 1, offset: 0 });
    for (const width of [1280, 390]) {
      await authPage.setViewportSize({ width, height: 844 });
      expect(await modal.locator('.modal-content').evaluate(el => el.scrollWidth <= el.clientWidth + 1)).toBe(true);
      await authPage.screenshot({ path: info.outputPath(`published-execution-${width}.png`) });
    }
    expect(errors).toEqual([]);
  });
});
