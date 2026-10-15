import * as path from 'path';

// 浏览器测试截图证据固定写入仓库根的 artifacts/test-results/ui/，
// 与运行入口的工作目录（仓库根或 test/browser）无关。
const UI_SCREENSHOT_DIR = path.resolve(__dirname, '..', '..', 'artifacts', 'test-results', 'ui');

export function uiScreenshotPath(fileName: string): string {
  return path.join(UI_SCREENSHOT_DIR, fileName);
}
