import {defineConfig,devices} from '@playwright/test';
export default defineConfig({testDir:'./suites',testMatch:/36-device-control-measurements\.spec\.ts/,outputDir:'../../artifacts/test-results/device-control-measurements',workers:1,retries:0,timeout:30000,reporter:[['list']],use:{...devices['Desktop Chrome']}});
