import {defineConfig} from '@playwright/test';
import {resolve} from 'node:path';
// Local reruns must never overwrite historical device acceptance evidence.
const out=process.env.COM15_ACTIONS_ARTIFACT_DIR||resolve(__dirname,'../../artifacts/com15-recovery-offline',new Date().toISOString().replace(/[:.]/g,'-')+'-'+process.pid);
export default defineConfig({testDir:'./suites',testMatch:/(35-device-control-layout|36-device-control-measurements|37-modbus-relay-status|38-modbus-relay-admission|40-device-control-actions|46-com15-stream-recovery|49-device-control-first-read|50-device-control-sse-lifecycle).spec.ts/,outputDir:resolve(out,'browser'),workers:1,retries:0,timeout:30000,reporter:[['list'],['json',{outputFile:resolve(out,'browser-results.json')}]],use:{browserName:'chromium',headless:true}});
