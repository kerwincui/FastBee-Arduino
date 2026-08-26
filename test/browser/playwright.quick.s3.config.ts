import { defineConfig } from '@playwright/test';

/**
 * FastBee-Arduino 蹇€熸祻瑙堝櫒娴嬭瘯閰嶇疆
 *
 * 鍙繍琛屽甫 @quick 鏍囩鐨勬牳蹇冩祴璇曠敤渚嬶紝閫傜敤浜庯細
 *   - 寮€鍙戦樁娈靛揩閫熼獙璇?
 *   - CI/CD 娴佹按绾垮揩閫熷洖褰?
 *   - 鍥轰欢鏇存柊鍚庡啋鐑熸鏌?
 *
 * 鐢ㄦ硶:
 *   cd test/browser
 *   DEVICE_IP=192.168.5.116 npx playwright test -c playwright.quick.config.ts
 *
 * 瀵规瘮鍏ㄩ噺閰嶇疆:
 *   | 鎸囨爣       | 鍏ㄩ噺 (273)  | 蹇€?(~62) |
 *   |------------|-------------|-----------|
 *   | 娴嬭瘯鏁伴噺    | 273         | ~62       |
 *   | 棰勪及鏃堕棿    | 30-37min    | 8-12min   |
 *   | screenshot | only-on-fail| only-on-fail|
 *   | trace      | off         | off       |
 *   | retries    | 1           | 1         |
 *   | 娴嬭瘯闂村欢杩? | 800ms       | 1200ms    |
 */

const deviceIp = process.env.DEVICE_IP || '192.168.1.1';
const devicePort = process.env.DEVICE_PORT || '80';
const interTestDelay = parseInt(process.env.INTER_TEST_DELAY_MS || '1200', 10);

export default defineConfig({
  testDir: './suites',
  outputDir: './test-results/quick-s3',
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: 1,         // 鍋跺彂瀵艰埅/鐧诲綍瓒呮椂濂椾欢鍐呰嚜鎰堬紙鐪熷疄鍥炲綊浼氳繛缁け璐?娆′粛鏆撮湶锛?
  workers: 1,
  timeout: 90_000,    // ESP32 涓茶浼犺緭涓嬪鑸?鐧诲綍鑰楁椂闀匡紝60s 浠嶅伓鍙戣秴闄愶紝鏀惧鍒?0s
  globalSetup: require.resolve('./stability-global-setup'),
  reporter: [
    ['list'],                                        // 瀹炴椂杩涘害
    ['json', { outputFile: 'reports/quick-s3-results.json' }],
    ['html', { outputFolder: 'reports/quick-s3-html', open: 'never' }],
  ],
  use: {
    baseURL: `http://${deviceIp}:${devicePort}`,
    screenshot: 'only-on-failure',
    trace: 'off',
    actionTimeout: 20_000,
    navigationTimeout: 45_000,
    extraHTTPHeaders: {
      'Accept-Language': 'zh-CN',
    },
  },
  projects: [
    {
      name: 'quick',
      // 鍖归厤鎵€鏈?spec 鏂囦欢锛岄€氳繃 --grep @quick 杩囨护
      testMatch: /\.spec\.ts$/,
      grep: /@quick/,
    },
  ],
});
