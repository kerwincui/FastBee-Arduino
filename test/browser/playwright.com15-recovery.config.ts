import {defineConfig} from '@playwright/test';
export default defineConfig({testDir:'./suites',testMatch:/46-com15-stream-recovery\.spec\.ts/,
 outputDir:'../../artifacts/test-results/com15-recovery',workers:1,retries:0,timeout:30000,
 reporter:[['list']],use:{browserName:'chromium',headless:true}});
