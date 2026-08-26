# Instructions

- Following Playwright test failed.
- Explain why, be concise, respect Playwright best practices.
- Provide a snippet of code with the fix, if possible.

# Test info

- Name: 05-peripheral.spec.ts >> Suite-05: 外设配置 >> PER-035: 完整 CRUD 流程 @quick
- Location: suites\05-peripheral.spec.ts:326:7

# Error details

```
Test timeout of 90000ms exceeded while running "beforeEach" hook.
```

# Page snapshot

```yaml
- generic [ref=e3]:
  - generic [ref=e4]:
    - generic [ref=e5]:
      - img "FastBee Logo" [ref=e6]
      - heading "设备管理系统" [level=3] [ref=e7]
    - generic [ref=e8]:
      - link "📊 设备监控" [ref=e9] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e10]: 📊
        - generic [ref=e11]: 设备监控
      - link "🛠️ 设备配置" [ref=e12] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e13]: 🛠️
        - generic [ref=e14]: 设备配置
      - link "🌐 网络设置" [ref=e15] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e16]: 🌐
        - generic [ref=e17]: 网络设置
      - link "🔧 外设配置" [ref=e18] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e19]: 🔧
        - generic [ref=e20]: 外设配置
      - link "⚡ 外设执行" [ref=e21] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e22]: ⚡
        - generic [ref=e23]: 外设执行
      - link "🔌 通信协议" [ref=e24] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e25]: 🔌
        - generic [ref=e26]: 通信协议
      - link "📺 设备大屏" [ref=e27] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e28]: 📺
        - generic [ref=e29]: 设备大屏
      - link "📜 规则脚本" [ref=e30] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e31]: 📜
        - generic [ref=e32]: 规则脚本
      - link "📜 设备日志" [ref=e33] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e34]: 📜
        - generic [ref=e35]: 设备日志
      - link "📁 文件管理" [ref=e36] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e37]: 📁
        - generic [ref=e38]: 文件管理
      - link "👥 用户管理" [ref=e39] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e40]: 👥
        - generic [ref=e41]: 用户管理
      - link "🧩 功能配置" [ref=e42] [cursor=pointer]:
        - /url: "#"
        - generic [ref=e43]: 🧩
        - generic [ref=e44]: 功能配置
  - generic [ref=e45]:
    - generic [ref=e46]:
      - generic [ref=e47]:
        - button "☰" [ref=e48] [cursor=pointer]
        - heading "设备监控仪表盘" [level=1] [ref=e49]
      - generic [ref=e50]:
        - combobox "语言/Language" [ref=e51] [cursor=pointer]:
          - option "中文" [selected]
          - option "English"
        - button "👤 管理员 ▼" [ref=e53] [cursor=pointer]:
          - generic [ref=e54]: 👤
          - generic [ref=e55]: 管理员
          - generic [ref=e56]: ▼
    - generic [ref=e59]: "© 2026 FastBee物联网平台 官网: www.fastbee.cn"
```

# Test source

```ts
  1   | ﻿import { test, expect, waitForDevice, waitForDeviceReady } from '../fixtures/base.fixture';
  2   | import { Locator, Page } from '@playwright/test';
  3   | 
  4   | /** 通过精确 value 选择下拉选项 */
  5   | async function selectByValue(locator: Locator, value: string) {
  6   |   await locator.selectOption(value);
  7   | }
  8   | 
  9   | /** 通过文本模糊匹配选择下拉选项 */
  10  | async function selectByPartialText(locator: Locator, text: string) {
  11  |   await locator.evaluate((el: HTMLSelectElement, t: string) => {
  12  |     const opt = Array.from(el.options).find(o => o.text.toLowerCase().includes(t.toLowerCase()));
  13  |     if (opt) el.value = opt.value;
  14  |   }, text);
  15  | }
  16  | 
  17  | /** 打开新增外设弹窗并等待就绪 */
  18  | async function openAddModal(page: Page) {
  19  |   // 等待 modals.html 片段加载完成（modal 元素存在于 DOM）
  20  |   await page.waitForSelector('#peripheral-modal', { state: 'attached', timeout: 20000 });
  21  |   // 先关闭可能残留的模态框
  22  |   const modal = page.locator('#peripheral-modal');
  23  |   if (await modal.isVisible().catch(() => false)) {
  24  |     await page.locator('#close-peripheral-modal').click({ timeout: 2000 }).catch(() => {});
  25  |     await page.waitForTimeout(500);
  26  |   }
  27  |   await page.click('#add-peripheral-btn');
  28  |   // 等待 modal 变为可见（带重试，嵌入式设备响应可能较慢）
  29  |   try {
  30  |     await expect(modal).toBeVisible({ timeout: 5000 });
  31  |   } catch {
  32  |     // 重试点击
  33  |     await page.click('#add-peripheral-btn');
  34  |     try {
  35  |       await expect(modal).toBeVisible({ timeout: 5000 });
  36  |     } catch {
  37  |       // JS 直接调用（事件委托可能尚未绑定）
  38  |       await page.evaluate(() => {
  39  |         if (typeof AppState !== 'undefined' && AppState.openPeripheralModal) {
  40  |           AppState.openPeripheralModal();
  41  |         }
  42  |       });
  43  |       await expect(modal).toBeVisible({ timeout: 8000 });
  44  |     }
  45  |   }
  46  | }
  47  | 
  48  | /** 在弹窗中选择外设类型 */
  49  | async function selectPeriphType(page: Page, value: string) {
  50  |   const typeSelect = page.locator('#peripheral-type-input');
  51  |   await selectByValue(typeSelect, value);
  52  |   await page.waitForTimeout(300);
  53  | }
  54  | 
  55  | /** 填写外设表单（名称 + 可选引脚） */
  56  | async function fillPeriphForm(page: Page, name: string, pins?: string) {
  57  |   const nameInput = page.locator('#peripheral-name-input');
  58  |   await nameInput.fill(name);
  59  |   if (pins !== undefined) {
  60  |     const pinsInput = page.locator('#peripheral-pins-input');
  61  |     await pinsInput.fill(pins);
  62  |   }
  63  | }
  64  | 
  65  | /** 保存外设弹窗并等待设备响应 */
  66  | async function savePeriphModal(page: Page) {
  67  |   const saveBtn = page.locator('#save-peripheral-btn');
  68  |   await saveBtn.click();
  69  |   // 等待 modal 关闭（表示保存成功）
  70  |   await page.locator('#peripheral-modal').waitFor({ state: 'hidden', timeout: 10_000 }).catch(() => {});
  71  |   await waitForDevice(page, 3000);
  72  | }
  73  | 
  74  | test.describe('Suite-05: 外设配置', () => {
  75  | 
> 76  |   test.beforeEach(async ({ authPage, navigateTo }) => {
      |        ^ Test timeout of 90000ms exceeded while running "beforeEach" hook.
  77  |     await navigateTo('peripheral');
  78  |   });
  79  | 
  80  |   // ========== 场景A: 页面结构与弹窗基础 ==========
  81  | 
  82  |   test('PER-001: 进入外设配置页 @quick', async ({ authPage }) => {
  83  |     await expect(authPage.locator('#peripheral-page')).toBeVisible();
  84  |     await expect(authPage.locator('#add-peripheral-btn')).toBeVisible();
  85  |     await expect(authPage.locator('#peripheral-refresh-btn')).toBeVisible();
  86  |     await expect(authPage.locator('#peripheral-filter-type')).toBeVisible();
  87  |   });
  88  | 
  89  |   test('PER-002: 列表表头完整性', async ({ authPage }) => {
  90  |     const table = authPage.locator('#peripheral-page table').first();
  91  |     await expect(table).toBeVisible();
  92  |     const headers = await table.locator('thead th').allTextContents();
  93  |     // 应包含: ID / 名称 / 类型 / 引脚 / 状态 / 操作
  94  |     expect(headers.length).toBeGreaterThanOrEqual(5);
  95  |   });
  96  | 
  97  |   test('PER-003: 新增外设弹窗打开', async ({ authPage }) => {
  98  |     await openAddModal(authPage);
  99  |     await expect(authPage.locator('#peripheral-modal')).toBeVisible();
  100 |     await expect(authPage.locator('#peripheral-form')).toBeVisible();
  101 |     await expect(authPage.locator('#peripheral-name-input')).toBeVisible();
  102 |     await expect(authPage.locator('#peripheral-type-input')).toBeVisible();
  103 |     await expect(authPage.locator('#peripheral-pins-input')).toBeVisible();
  104 |     await expect(authPage.locator('#peripheral-modal .toggle-switch').first()).toBeVisible(); // toggle wraps checkbox, checkbox itself is hidden
  105 |   });
  106 | 
  107 |   test('PER-004: 关闭弹窗按钮', async ({ authPage }) => {
  108 |     await openAddModal(authPage);
  109 |     await authPage.locator('#close-peripheral-modal').click();
  110 |     await expect(authPage.locator('#peripheral-modal')).not.toBeVisible({ timeout: 5000 });
  111 |   });
  112 | 
  113 |   test('PER-005: 取消按钮关闭弹窗', async ({ authPage }) => {
  114 |     await openAddModal(authPage);
  115 |     await authPage.locator('#cancel-peripheral-btn').click();
  116 |     await expect(authPage.locator('#peripheral-modal')).not.toBeVisible({ timeout: 5000 });
  117 |   });
  118 | 
  119 |   // ========== 场景B: 类型下拉完整性（合并为单测试 + test.step） ==========
  120 | 
  121 |   test('PER-010~013: 外设类型下拉完整性（通信/GPIO/模拟/专用）', async ({ authPage }) => {
  122 |     const typeSelect = authPage.locator('#peripheral-type-input');
  123 | 
  124 |     await test.step('通信接口: UART(1), I2C(2), SPI(3)', async () => {
  125 |       await openAddModal(authPage);
  126 |       const options = await typeSelect.locator('option').evaluateAll((els: Element[]) => els.map(e => (e as HTMLOptionElement).value));
  127 |       expect(options).toContain('1');
  128 |       expect(options).toContain('2');
  129 |       expect(options).toContain('3');
  130 |       await authPage.locator('#cancel-peripheral-btn').click();
  131 |       await authPage.waitForTimeout(500);
  132 |     });
  133 | 
  134 |     await test.step('GPIO接口: 数字输入(11), 数字输出(12), PWM输出(17), 触摸(21)', async () => {
  135 |       await openAddModal(authPage);
  136 |       const options = await typeSelect.locator('option').evaluateAll((els: Element[]) => els.map(e => (e as HTMLOptionElement).value));
  137 |       expect(options).toContain('11');
  138 |       expect(options).toContain('12');
  139 |       expect(options).toContain('17');
  140 |       expect(options).toContain('21');
  141 |       await authPage.locator('#cancel-peripheral-btn').click();
  142 |       await authPage.waitForTimeout(500);
  143 |     });
  144 | 
  145 |     await test.step('模拟信号: ADC(26), DAC(27)', async () => {
  146 |       await openAddModal(authPage);
  147 |       const options = await typeSelect.locator('option').evaluateAll((els: Element[]) => els.map(e => (e as HTMLOptionElement).value));
  148 |       expect(options).toContain('26');
  149 |       expect(options).toContain('27');
  150 |       await authPage.locator('#cancel-peripheral-btn').click();
  151 |       await authPage.waitForTimeout(500);
  152 |     });
  153 | 
  154 |     await test.step('专用外设: 编码器(43), WS2812B(45), TM1637(47), RF(48), 雷达(49), 红外接收头(53), 设备事件(60)', async () => {
  155 |       await openAddModal(authPage);
  156 |       const options = await typeSelect.locator('option').evaluateAll((els: Element[]) => els.map(e => (e as HTMLOptionElement).value));
  157 |       expect(options).toContain('43');
  158 |       expect(options).toContain('45');
  159 |       expect(options).toContain('47');
  160 |       expect(options).toContain('48');
  161 |       expect(options).toContain('49');
  162 |       expect(options).toContain('53');
  163 |       expect(options).toContain('60');
  164 |       await authPage.locator('#cancel-peripheral-btn').click();
  165 |       await authPage.waitForTimeout(500);
  166 |     });
  167 |   });
  168 | 
  169 |   // ========== 场景C: 类型切换参数联动（合并为单测试 + test.step） ==========
  170 | 
  171 |   test('PER-037~042g: 类型切换参数区域联动（全类型）', async ({ authPage }) => {
  172 | 
  173 |     await test.step('UART → UART参数可见, GPIO隐藏', async () => {
  174 |       await openAddModal(authPage);
  175 |       await selectPeriphType(authPage, '1');
  176 |       await expect(authPage.locator('#uart-params')).toBeVisible();
```