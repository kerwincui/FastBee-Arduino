import { defineConfig } from '@playwright/test';
import { resolve } from 'node:path';
const artifacts = process.env.COM15_ACTIONS_ARTIFACT_DIR || resolve(__dirname, '../../artifacts/com15-execution-cards-20260930');
export default defineConfig({
  testDir: './suites',
  testMatch: /(35-device-control-layout|36-device-control-measurements|40-device-control-actions|46-com15-stream-recovery).spec.ts/,
  outputDir: resolve(artifacts, 'browser'),
  workers: 1, retries: 0, timeout: 30000,
  reporter: [['list'], ['json', { outputFile: resolve(artifacts, 'browser-results.json') }]],
  use: { browserName: 'chromium', headless: true }
});
