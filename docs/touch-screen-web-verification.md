# 触控屏 Web 优化本地验证记录

日期：2026-09-22。范围：Web 源码、模拟 API、浏览器与构建资源契约；没有连接设备或执行实物电机。

## 已完成行为

- 设备屏幕菜单移到通信协议后；协议页面只由 MQTT / Modbus 能力决定。退休远程资源分片不再发布；旧入口跳到屏幕设置。
- 预览默认显示已保存配置，所有控制只读；配置集中管理页面来源、单一心知天气、外观、亮度和息屏。高级属性面板首次展开时创建，初始页面只有 199 个静态节点。
- 根据真实来源生成首页、温湿度趋势、电机、系统页面；缺少电机时不生成空电机卡，没有开关待配置占位。
- 电机组件保存完整参数及步长，保留数值 0；亮度使用 range，明确不支持连续调光时禁用并显示“仅支持背光开关”。自动息屏复用实际 sleep 能力。
- 天气 API Key 输入为空保留服务器密钥，密钥不写入布局与 localStorage；刷新保留未保存天气输入。默认更新周期 1800 秒，失败与限流显示真实错误。
- 保存天气 / 历史 / 屏幕时逐组记录已成功部分，409 冲突保留布局草稿和具体提示。趋势来源并入既有历史 allowlist，不删除未知旧来源。保存期间禁止编辑以防覆盖新输入。
- 远程通道 action 33 不再出现在新动作选项，旧动作仍可辨认并明确标注已停用，要求删除或替换；action 34 只选择已启用心知天气。

## 预算证据

保持总预算 **315 KiB**、启动预算 **66 KiB**、单运行时模块 **16 KiB**、屏幕静态 DOM **220** 不变。屏幕原分配 lazy 31 KiB + shared 5 KiB 调整为 lazy 34.5 KiB + shared 1.5 KiB，总分配仍为 36 KiB。

使用任务开始时保存的 `artifacts/touch-optimization-20260922/before/` 源码，按生产协议 bundle 顺序和相同 Terser 参数（compress:false、mangle:true）及 gzip level 9 重建对照：

| 对照 | gzip 字节 |
| --- | ---: |
| 基线 protocol.js，含旧远程编辑器 | 16,350 |
| 相同基线只移除旧远程编辑器 | 9,628 |
| 此编辑器边际回收 | 6,722 |

重分配的 3,584 字节小于仅协议编辑器回收量，尚未计入删除的 HTML 分片。设置方法整合到已有 model 资源，不增加请求。高级属性表单按需创建；没有扩大 DOM 门。

2026-09-22 本地质量门通过时全站有效资源约 312.4 KiB、启动 62.5 KiB。最终数值应以完整 Host / 固件构建生成的最新报告为准，避免把阶段构建当成最终固件证据。

## 本地测试

- `test_screen_panel_web_runtime` 及 presentation 的 source/minified Web 运行用例：7 项通过。
- `node scripts/test-web-profile.js`：9 个固件环境能力契约通过。
- `node scripts/test-web-quality-gate.js`：预算正反边界与发布资源检查通过。
- `node scripts/test-web-capability-build.js`：11 次干净 Web 构建通过；日志 `artifacts/touch-optimization-20260922/web-capability.log`。
- Playwright suites 31 / 32：12 项完整通过（29.7 秒）。覆盖只读预览、四类模板、桌面和手机无横向溢出、旋转与逆向拖拽、历史未知引用保留、409 草稿保留、密钥留空保留、天气限流、菜单顺序、DOM 重建、部分成功及旧远程动作退休。
- 扩展回归：35 项通过（1.2 分钟），包含上述 12 项、suite 26 生命周期、既有布局回归，以及 suite 30 的协议 / 能力契约在 **4 个代表设备配置 × 2 个视口** 下验证。离线 fixture 只有 ESP32 / C3 / C6 / S3 四个代表配置，不把此结果称作九种实物验证。
- 新增 `WEB-PANEL-MOTOR`：1 项通过（3.7 秒）。生产大屏真实 DOM 连续进入 / 离开三次，每次正转点击恰好一条请求；未提交参数阻止反转，设置速度后反转恰好一条请求；状态保持“运行状态未确认”。请求只发送给 loopback 模拟服务器。
- 单独天气默认周期 1800 秒回归：通过。

截图在 `test/browser/artifacts/screen-panel/`：`smart-panel-desktop.png`、`smart-panel-mobile.png`、`config-desktop.png`、`config-mobile.png`、`motor-preview.png`，及 360 / 390 像素布局截图。人工检查后进一步将时钟字号按卡片高度缩放，避免手机预览提示被裁切；最终两条预览回归通过，手机时钟文字无纵向裁切。

实物触控、唤醒、总线电机动作、天气联网、持续运行由硬件阶段验收，本记录不作这些通过声明。

## 外设执行弹窗及最终补充

按新需求保留启用、执行后上报、最近 10 条采样、面板控制各自语义及默认权限，移除拥挤的横排，将规则名和启用置于基本信息行；其他选项分成数据与记录 / 屏幕操作权限两组，390 像素单列显示。最近记录使用新标签页进入设备屏幕配置的历史区域，原弹窗草稿不关闭、不重置。1280 / 390 像素截图已人工核查：`test-results/ui/periph-rule-options-1280.png`、`test-results/ui/periph-rule-options-390.png`。

最终 3 条浏览器回归通过（11.8 秒），覆盖手机时钟无裁切、页面 DOM 重建、天气与布局部分保存，以及新弹窗布局 / 默认值 / 新标签页保留草稿 / action 34 保存。日志：`artifacts/touch-optimization-20260922/web-final-visual.log`。

`test-web-publish-lock.js` 修复固定 200 毫秒等待引起的竞争测试偶发失败：等待所有真实写入进程输出锁竞争状态再释放 holder（20 秒上限），先持有外层 firmware 锁供嵌套 gzip 借用，异常路径先释放并回收进程，再清理临时目录。专项通过，日志：`artifacts/touch-optimization-20260922/web-publish-lock-final.log`。所有持锁测试已结束并释放锁。

## 最终首页导航的隔离浏览器回归

最终生产源码冻结后，在九目标固件构建并行期间执行；没有运行共享 Web 发布命令，没有改写 `data/www`、`.pio`，没有申请或抢占构建锁。

- 将当时 `esp32s3-F16R8` 的 57 个发布文件复制到 `artifacts/touch-optimization-20260922/isolated-browser/www`，逐个验证复制前后 SHA-256 一致。
- 在隔离目录用与生产相同的 Terser 参数及共享 nameCache 重建最终 `screen-panel-model.js`、`screen-panel.js`，用生产 CSS 压缩规则重建 `screen-panel.css`，均 gzip level 9。其他资产保持已发布快照。
- 复制的模拟服务只访问隔离资源，构建分支强制禁用，在 `127.0.0.2:41745` 运行。首次快照脚本因 Windows 正反斜线比较造成 404，已在复制的服务中使用规范化绝对路径修复；没有改动生产服务器。
- suites **26 / 31 / 32 共 16 项全部通过（2.3 分钟）**，包括固定首页导航、导航不发执行请求、预览卡片内容区缩放、桌面 / 手机布局、时钟不裁切、电机单次提交及参数草稿、天气/权限/表单相关行为。最终手机截图人工确认首页导航可见且不遮挡卡片。
- 命令：`OFFLINE_UI_SKIP_BUILD=1 OFFLINE_UI_PORT=41745 node test/browser/node_modules/@playwright/test/cli.js test -c artifacts/touch-optimization-20260922/isolated-browser/playwright.config.ts --project=offline-ui "26-device-control-lifecycle|31-screen-panel|32-periph-exec-remote"`（Windows 下用 PowerShell `$env:` 设置两项环境变量）。
- 日志：`artifacts/touch-optimization-20260922/isolated-browser.log`；57 份输入/输出 SHA、3 份最终源码 SHA、Terser 版本及压缩参数、复制的服务/配置 SHA：`artifacts/touch-optimization-20260922/isolated-browser/snapshot.json`。
- 最终 Host 发布后执行 `node artifacts/touch-optimization-20260922/isolated-browser/compare-published.cjs`，比较全部 57 个发布资产与已验证快照是否逐字节一致，包括共享样式、主包、菜单和屏幕模块。新增、缺失及元数据差异分别报告；不豁免正文差异。仅一致时复用上述浏览器结论；不一致则重新核对对应模块影响。

本轮结果为**最终屏幕源码 + 其他已发布资产快照的隔离浏览器验证**，不能代替最终 Host 发布一致性检查或实物验收。

## 最终 Host 发布资源核对与补充回归

Host 完成 Web 发布、进入 unit 阶段后，对最终 `data/www` 执行全部 **57 项**严格 SHA-256 比对：**53 项相同、4 项不同，无新增或缺失**。严格比较按设计返回不一致，未将差异忽略后宣称 57 项完全相同。日志：`artifacts/touch-optimization-20260922/final-published-57-compare.log`。

逐项核对差异如下：

| 资产 | 相对隔离浏览器快照的变化 | 核对结果 |
| --- | --- | --- |
| `js/modules/protocol.js.gz` | 9,602 → 9,622 B | 仅新增局部翻译函数 `t` 回退 |
| `js/modules/protocol-modbus-rtu.js.gz` | 6,517 → 6,547 B | 仅新增局部翻译函数 `t` 回退；删除事务等已有改动已包含在旧快照 |
| `index.html.gz` | 资源版本号 `20260922-1742-62787673` → `20260922-1826-d9cde29f` | 仅版本号变化 |
| `sw.js.gz` | 缓存版本号同步变化 | 仅版本号变化 |

协议 bundle 在内存中按生产拼接顺序、Terser / minifyJS 参数重新生成：当前源码等于最终发布解压内容，移除上述两行回退后等于旧快照解压内容，均逐字节验证。协议压缩资源合计仅增加 **50 B**，未改变屏幕分配预算；所有屏幕资产、共享样式、主包和菜单资产与已验证快照一致。

随后直接读取最终发布目录，在隔离端口 `127.0.0.2:41746` 运行 **10 项回归，全部通过（26.0 秒）**：

- `WEB-PANEL-012`：通信协议与设备屏幕往返、菜单顺序、重新挂载、电机参数和部分保存。
- `WEB-MATRIX-protocol`：ESP32 / C3 / C6 / S3 四个代表模拟配置 × 桌面 / 手机两个视口，共 8 项，验证最终发布协议页面的显示、能力限制与布局。
- `34-protocol-direct-entry`：1 项源脚本测试，独立验证没有全局 `t` 或外设页预加载时 Modbus 首次打开与失败提示。此项读取源脚本，不将其称为发布 bundle 浏览器验证。

命令：`OFFLINE_UI_SKIP_BUILD=1 OFFLINE_UI_PORT=41746 node test/browser/node_modules/@playwright/test/cli.js test -c artifacts/touch-optimization-20260922/final-published-browser.config.ts`（PowerShell 用 `$env:` 设置环境变量）。配置强制要求跳过构建；服务仅从 `data/www` 读取资源，没有写入该目录或 `.pio`，没有获取构建锁。

日志：`artifacts/touch-optimization-20260922/final-published-browser.log`。测试开始前记录全部 57 个发布资产 SHA-256，结束后再核对，**全部不变**；记录分别在 `final-published-browser-input.json` 与 `final-published-browser-evidence.json`。结合原 16 项屏幕/外设浏览器回归、53 项未变资源和本次受影响发布页面回归，完成当前 Web 产物的本地验证闭环；仍不代表真实设备、固件构建矩阵或硬件验收通过。
