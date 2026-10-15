import { defineConfig, devices } from '@playwright/test';
import { readFileSync } from 'node:fs';
import path from 'node:path';

const host = process.env.OFFLINE_UI_HOST || '127.0.0.2';
const port = process.env.OFFLINE_UI_PORT || '41739';
const baseURL = `http://${host}:${port}`;
const artifactRoot = process.env.FASTBEE_TEST_ARTIFACT_ROOT?.trim();
const suiteArtifacts = artifactRoot ? path.resolve(artifactRoot, 'offline-ui') : '';
const profileFixture = JSON.parse(readFileSync(
  path.join(__dirname, 'offline-device-profiles.json'),
  'utf8'
)) as { profiles: Record<string, unknown> };

const matrixViewports = [
  { key: 'desktop', viewport: { width: 1440, height: 900 } },
  { key: 'mobile', viewport: { width: 390, height: 844 } },
];

const matrixProjects = Object.keys(profileFixture.profiles).flatMap((profile) =>
  matrixViewports.map(({ key, viewport }) => ({
    name: `matrix-${profile}-${key}`,
    testMatch: /30-offline-target-matrix\.spec\.ts/,
    metadata: { offlineProfile: profile, offlineViewport: key },
    use: {
      ...devices['Desktop Chrome'],
      viewport,
      serviceWorkers: 'block' as const,
      extraHTTPHeaders: {
        'Accept-Language': 'zh-CN',
        'X-FastBee-Offline-Profile': profile,
      },
    },
  }))
);

process.env.DEVICE_BASE_URL = baseURL;
process.env.TEST_DELAY_MS = process.env.TEST_DELAY_MS || '0';

export default defineConfig({
  testDir: './suites',
  outputDir: suiteArtifacts ? path.join(suiteArtifacts, 'browser') : '../../artifacts/test-results/offline-ui',
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: 0,
  workers: 1,
  timeout: 240_000,
  reporter: suiteArtifacts ? [['list'], ['json', { outputFile: path.join(suiteArtifacts, 'browser-results.json') }]] : [['list']],
  grep: /UI-005E|UI-005F|UI-005G|UI-007A|UI-007C|UI-031|UI-025A|UI-026A|UI-027|FCFG-004A|WEB-(?:SEC|UX|GENERIC|MATRIX|PANEL)-|WEB-00[1-4]|AI-/,
  webServer: {
    command: 'node offline-ui-server.mjs',
    url: `${baseURL}/api/health`,
    timeout: 120_000,
    reuseExistingServer: false,
  },
  use: {
    ...devices['Desktop Chrome'],
    baseURL,
    screenshot: 'only-on-failure',
    trace: 'retain-on-failure',
    actionTimeout: 20_000,
    navigationTimeout: 30_000,
    extraHTTPHeaders: { 'Accept-Language': 'zh-CN' },
  },
  projects: [
    {
      name: 'offline-ui',
      testMatch: /15-ui-regression\.spec\.ts|22-feature-config\.spec\.ts|24-web-smooth-access\.spec\.ts|26-device-control-lifecycle\.spec\.ts|27-web-security-lifecycle\.spec\.ts|29-generic-peripheral-integration\.spec\.ts|31-screen-panel\.spec\.ts|32-periph-exec-remote\.spec\.ts|42-screen-panel-recovery\.spec\.ts/,
    },
    ...matrixProjects,
  ],
});
