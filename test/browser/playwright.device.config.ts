import { defineConfig, type ReporterDescription } from '@playwright/test';
import path from 'node:path';
import { constrainedDevicePolicy } from './constrained-device-policy';
import baseConfig from './playwright.config';

/**
 * Real-device smoke acceptance.
 *
 * This keeps the regular smoke selection while applying a fail-fast policy
 * suitable for constrained targets. Authentication state remains in process
 * memory only; no password or reusable session file is written to disk.
 */
const smokeProject = baseConfig.projects?.find((project) => project.name === 'smoke');
if (!smokeProject) {
  throw new Error('The base Playwright configuration must define the smoke project.');
}

const externalOutputDir = process.env.FASTBEE_PLAYWRIGHT_OUTPUT_DIR?.trim();
const externalReportDir = process.env.FASTBEE_PLAYWRIGHT_REPORT_DIR?.trim();
const reporters: ReporterDescription[] = externalReportDir
  ? [
      ['list'],
      ['json', { outputFile: path.join(externalReportDir, 'results.json') }],
      ['html', { outputFolder: path.join(externalReportDir, 'html'), open: 'never' }],
    ]
  : [
      ['list'],
      ['json', { outputFile: 'reports/device-results.json' }],
      ['html', { outputFolder: 'reports/device-html', open: 'never' }],
    ];

export default defineConfig({
  ...baseConfig,
  ...constrainedDevicePolicy,
  outputDir: externalOutputDir || './test-results/device',
  reporter: reporters,
  projects: [smokeProject],
});
