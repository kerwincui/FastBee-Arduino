import { defineConfig } from '@playwright/test';
export default defineConfig({testDir:'./suites',testMatch:/38-modbus-relay-admission\.spec\.ts/,
 outputDir:'./test-results/modbus-relay-admission',workers:1,retries:0,reporter:[['list']],
 use:{browserName:'chromium',headless:true}});
