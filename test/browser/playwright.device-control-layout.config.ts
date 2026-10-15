import {defineConfig,devices} from '@playwright/test';
export default defineConfig({testDir:'./suites',testMatch:/35-device-control-layout\.spec\.ts/,outputDir:'../../artifacts/test-results/device-control-layout',workers:1,retries:0,timeout:30000,reporter:[['list']],use:{...devices['Desktop Chrome']}});
