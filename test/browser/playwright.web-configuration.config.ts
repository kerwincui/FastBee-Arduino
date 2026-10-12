import { defineConfig } from '@playwright/test';
import path from 'node:path';

const port = process.env.WEB_CONFIGURATION_PORT || '41741';
const baseURL = 'http://127.0.0.2:' + port;
const artifactRoot = process.env.FASTBEE_TEST_ARTIFACT_ROOT?.trim();
const suiteArtifacts = artifactRoot ? path.resolve(artifactRoot, 'web-configuration') : '';
process.env.OFFLINE_UI_PORT = port;
process.env.DEVICE_BASE_URL = baseURL;
process.env.DEVICE_SERIAL = '';
process.env.OFFLINE_UI_SKIP_BUILD = '1';
if (!process.env.OFFLINE_UI_WEB_ROOT) throw new Error('Isolated assets required; run node scripts/test-web-configuration.js');

export default defineConfig({
  testDir: './suites',
  testMatch: /(?:35-device-control-layout|36-device-control-measurements|37-modbus-relay-status|38-modbus-relay-admission|39-device-ota-config|40-device-control-actions|41-device-config-functions|42-web-configuration|43-peripheral-completeness|44-ota-failure-lifecycle|45-configuration-lifecycle|46-modbus-instrument-template|47-shared-execution-ui)\.spec\.ts/,
  outputDir: suiteArtifacts ? path.join(suiteArtifacts, 'browser') : './test-results/web-configuration',
  workers: 1, retries: 0, timeout: 90000,
  reporter: [['list'], ['json', { outputFile: suiteArtifacts ? path.join(suiteArtifacts, 'browser-results.json') : (process.env.WEB_CONFIGURATION_OUTPUT || '../../artifacts/web-configuration-20260929') + '/browser-results.json' }]],
  webServer: { command: 'node offline-ui-server.mjs', url: baseURL + '/api/health', reuseExistingServer: false, timeout: 30000 },
  use: { browserName: 'chromium', baseURL, headless: true, serviceWorkers: 'block', actionTimeout: 15000, trace: 'retain-on-failure', screenshot: 'only-on-failure' },
});
