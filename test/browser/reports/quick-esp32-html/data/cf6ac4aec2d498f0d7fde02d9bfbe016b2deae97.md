# Instructions

- Following Playwright test failed.
- Explain why, be concise, respect Playwright best practices.
- Provide a snippet of code with the fix, if possible.

# Test info

- Name: 12-files.spec.ts >> Suite-12: 文件管理 >> FILE-010: 导入配置按钮存在 @quick
- Location: suites\12-files.spec.ts:91:7

# Error details

```
Test timeout of 90000ms exceeded while running "beforeEach" hook.
```

# Test source

```ts
  1   | ﻿import { test, expect, waitForDevice, hasMenuPage } from '../fixtures/base.fixture';
  2   | 
  3   | test.describe('Suite-12: 文件管理', () => {
  4   | 
> 5   |   test.beforeEach(async ({ authPage, navigateTo }) => {
      |        ^ Test timeout of 90000ms exceeded while running "beforeEach" hook.
  6   |     test.skip(!(await hasMenuPage(authPage, 'data')), '设备不支持文件管理页面');
  7   |     await navigateTo('data');
  8   |   });
  9   | 
  10  |   // ========== 页面加载与展示 ==========
  11  | 
  12  |   test('FILE-001: 进入文件管理页 @quick', async ({ authPage }) => {
  13  |     await expect(authPage.locator('#data-page')).toBeVisible();
  14  |     await expect(authPage.locator('#data-page .fb-card-title')).toContainText(/文件管理|File/);
  15  |   });
  16  | 
  17  |   test('FILE-002: 文件系统信息展示', async ({ authPage }) => {
  18  |     const fsInfo = authPage.locator('#fs-info');
  19  |     await expect(fsInfo).toBeVisible();
  20  |     // 应显示容量信息
  21  |     const text = await fsInfo.textContent();
  22  |     expect(text).toBeTruthy();
  23  |   });
  24  | 
  25  |   test('FILE-003: 文件树展示', async ({ authPage }) => {
  26  |     const fileTree = authPage.locator('#file-tree');
  27  |     await expect(fileTree).toBeVisible();
  28  |     // 等待加载完成（"加载中..."消失）
  29  |     await authPage.waitForTimeout(3000);
  30  |     const treeContent = await fileTree.innerHTML();
  31  |     // 应有文件或目录条目
  32  |     expect(treeContent).toBeTruthy();
  33  |   });
  34  | 
  35  |   test('FILE-004: 当前目录路径显示', async ({ authPage }) => {
  36  |     const currentDir = authPage.locator('#current-dir-path');
  37  |     await expect(currentDir).toBeVisible();
  38  |     const dirText = await currentDir.textContent();
  39  |     // 应显示路径（至少根目录 /）
  40  |     expect(dirText).toBeTruthy();
  41  |   });
  42  | 
  43  |   // ========== 文件操作按钮 ==========
  44  | 
  45  |   test('FILE-005: 刷新按钮 @quick', async ({ authPage }) => {
  46  |     const refreshBtn = authPage.locator('#fs-refresh-btn');
  47  |     await expect(refreshBtn).toBeVisible();
  48  |     await refreshBtn.click();
  49  |     await authPage.waitForTimeout(2000);
  50  |     // 刷新后页面不崩溃
  51  |     await expect(authPage.locator('#data-page')).toBeVisible();
  52  |   });
  53  | 
  54  |   test('FILE-006: 返回上级按钮', async ({ authPage }) => {
  55  |     const upBtn = authPage.locator('#fs-up-btn');
  56  |     await expect(upBtn).toBeVisible();
  57  |     // 在根目录点击返回上级不应出错
  58  |     await upBtn.click();
  59  |     await authPage.waitForTimeout(1000);
  60  |     await expect(authPage.locator('#data-page')).toBeVisible();
  61  |   });
  62  | 
  63  |   // ========== 文件编辑器 ==========
  64  | 
  65  |   test('FILE-007: 文件编辑器初始状态', async ({ authPage }) => {
  66  |     const editor = authPage.locator('#file-editor');
  67  |     await expect(editor).toBeVisible();
  68  |     // 初始状态应禁用（未选择文件）
  69  |     const isDisabled = await editor.isDisabled();
  70  |     expect(isDisabled).toBeTruthy();
  71  |   });
  72  | 
  73  |   test('FILE-008: 文件路径提示', async ({ authPage }) => {
  74  |     const filePath = authPage.locator('#current-file-path');
  75  |     await expect(filePath).toBeVisible();
  76  |     // 初始应显示"请选择文件"
  77  |     const text = await filePath.textContent();
  78  |     expect(text).toBeTruthy();
  79  |   });
  80  | 
  81  |   // ========== 配置文件导出/导入 ==========
  82  | 
  83  |   test('FILE-009: 导出配置按钮存在', async ({ authPage }) => {
  84  |     const exportBtn = authPage.locator('#fs-export-config-btn');
  85  |     // 按钮可能存在（取决于页面状态）
  86  |     if (await exportBtn.isVisible()) {
  87  |       await expect(exportBtn).toBeVisible();
  88  |     }
  89  |   });
  90  | 
  91  |   test('FILE-010: 导入配置按钮存在 @quick', async ({ authPage }) => {
  92  |     const importBtn = authPage.locator('#fs-import-config-btn');
  93  |     if (await importBtn.isVisible()) {
  94  |       await expect(importBtn).toBeVisible();
  95  |     }
  96  |   });
  97  | 
  98  |   // ========== 文件树交互 ==========
  99  | 
  100 |   test('FILE-011: 点击文件树条目', async ({ authPage }) => {
  101 |     await authPage.waitForTimeout(3000); // 等待文件树加载
  102 |     // 尝试点击第一个文件/目录条目
  103 |     const firstItem = authPage.locator('#file-tree .fs-item, #file-tree .fs-file, #file-tree .fs-dir, #file-tree li, #file-tree [data-name]').first();
  104 |     if (await firstItem.isVisible()) {
  105 |       await firstItem.click();
```