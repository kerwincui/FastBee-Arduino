import { defineConfig } from '@playwright/test';
export default defineConfig({ testDir:'./suites', testMatch:/37-modbus-relay-status\.spec\.ts/,
 outputDir:'./test-results/modbus-relay-status',workers:1,retries:0,reporter:[['list']],
 use:{browserName:'chromium',headless:true} });
