# FastBee 触控屏与 Web 配置体验 V2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在不破坏 FastBee 通用固件配置模型的前提下，将设备屏幕 Web 编辑器重构为简洁的三栏可视化编辑器，并修复触控导航、手势冲突、天气、电机、亮度、息屏和蜂鸣器的实际可用性问题。

**Architecture:** 保持“外设配置提供硬件能力、外设执行提供数据与动作、设备屏幕只保存布局和安全引用”的三层边界。Web 采用“预览 / 配置”双 Tab 和 A 方案三栏编辑器；实体屏幕使用统一的页面导航、手势仲裁及能力驱动系统控件。所有硬件差异由显示/触摸/蜂鸣器后端能力描述和外设配置承担，不在页面组件中写死板型、GPIO 或寄存器。

**Tech Stack:** ESP32 Arduino、PlatformIO、C++、LittleFS/ArduinoJson、原生 JavaScript/HTML/CSS、U8g2、StripeRenderer、Playwright、Python/Native C++ 测试。

---

## 一、计划定位与当前基线

这是已落地触控屏优化后的第二轮 UX 与实物缺陷收敛计划。当前代码已经具备菜单迁移、心知天气、预览/配置双 Tab、五槽底部导航、电机公共服务、亮度滑块、息屏唤醒和蜂鸣器关闭重试等实现；截图和现场反馈说明“代码存在”不等于“设备可用”。本轮先核对部署版本、运行配置和硬件能力，再针对可复现问题修改，避免重复重写已完成模块。

当前已确认的界面问题：

1. 预览页底部仍显示导入、导出、撤销和“保存并应用”，破坏“预览只查看”的职责。
2. 配置页是长纵向表单，页面管理、运行状态、天气、外观、息屏、权限、历史和来源入口平铺，信息层级弱、空白过多。
3. 页面工具按钮和保存入口过多；历史设置仍有单独保存按钮，与全局保存冲突。
4. Tab 紧贴卡片顶部；虽然已有 `sp-body` padding，但 `sp-top-tabs` 的 sticky/边距组合仍产生视觉贴边。
5. 预览和实体页面虽然已有底栏代码，但必须核对设备部署版本和五个页面的真实命中范围。
6. 亮度逻辑源码已按 10%–100%可见范围设计，现场却出现 100% 黑、0%最亮，优先判断为显示后端极性/控制芯片语义或部署配置不匹配，不能仅颠倒 UI 数值。
7. “Waiting for NTP before TLS”说明天气任务被安全 TLS 的时间准入阻塞；需修复自动恢复和中文状态，不允许关闭证书校验规避。
8. 蜂鸣器已有息屏前关闭逻辑，但现场持续鸣叫说明启动默认电平、CH32 位掩码、I2C 写失败恢复或固件版本仍需逐层验证。

## 二、审核后采用的产品决策

### 2.1 Web 页面结构

- 左侧菜单顺序保持“外设执行 → 通信协议 → 设备屏幕 → 设备大屏”。
- 通信协议仅保留 MQTT 与 Modbus RTU；不恢复“远程资源”Tab。
- 设备屏幕只保留两个一级 Tab：
  - **预览**：显示已保存配置或明确标记的草稿效果，只切页、刷新数据和进入编辑，不执行真实电机、息屏或重启。
  - **配置**：采用已确认的 A 方案三栏编辑器，唯一主保存入口为“保存并应用”。
- 配置页桌面布局：左侧“页面 + 组件库”，中间“等比例屏幕画布”，右侧“所选页面/组件属性”。
- 全局配置从长页面移入“全局设置”抽屉，分为“显示与外观、心知天气、触摸与息屏、安全操作”四组。
- 手机/窄屏不压缩三栏：改为页面列表、画布、属性抽屉分步显示；底部保存条不遮挡内容。

### 2.2 实体触控导航与手势

- 480×480 屏幕底部固定 72 px 导航区，完整作为点击热区；显示最多五个直接页面入口。
- 恰好五页时全部显示；超过五页时显示四个固定入口和“更多”，其他页面仍可滑动访问。
- 导航项由 Web 配置页面顺序决定，可配置显示/隐藏；默认首页始终可达。
- 普通数据卡、图表卡和控制卡容器本身不响应点击；只有明确按钮、滑块、页面入口等子控件可执行动作。
- 横向移动超过阈值后，手势归属页面滑动并取消点击；按钮只有“按下命中、抬起仍在按钮内、移动未超阈值”才触发。
- 实体趋势图默认不响应单击，避免抢占左右滑动；图中直接提供时间轴、数值轴、单位和最新点详情。Web 预览可显示鼠标悬停说明，但不得下发控制。
- 息屏后的第一次完整触摸只负责唤醒，直到抬手前不允许导航、按钮、电机或提示音动作。

### 2.3 电机控制

- 电机页显示：设备名、在线/离线、命令状态、可回读状态、设定速度、单次脉冲、正转、停止、反转。
- 配置页只选择已注册且声明电机能力的外设/规则，不允许屏幕布局保存任意寄存器地址。
- 首次正转/反转显示操作摘要并要求确认；确认后的信任窗口内直接执行，默认 60 秒，Web 可在 15–600 秒内调整；闲置超时后重新提示。停止始终直接执行且优先。
- 参数编辑与命令分离：修改速度/脉冲不会启动，提交成功后才能用于下一次启动。
- HTTP 成功只表示命令被接受；无方向反馈时显示“指令已发送，运行状态未确认”，不能伪造“运行中”。
- Web 大屏与触控共用 `MotorControlService`，以请求 ID、单设备在途锁、无自动重发的运动命令和事件来源防回流保证一次操作只有一次启动语义。

### 2.4 亮度、息屏与提示音

- 用户亮度语义统一为 1%最暗、100%最亮；0 不作为普通亮度，仅由息屏状态表示。
- 可见最低亮度默认 10%，但由显示后端 capability 给出，可按硬件配置调整；不允许用户拖到不可恢复的全黑。
- `backlightActiveHigh` 只描述实际硬件极性；Web 和屏幕逻辑永远不反向解释百分比。
- 立即息屏使用独立按钮，不让外层卡片可点击；触摸唤醒恢复息屏前亮度。
- 触摸提示音为全局布尔配置，只在硬件声明 buzzer capability 时显示。仅有效按钮/导航在释放后短鸣；滑动、拖动、唤醒和立即息屏不鸣叫。
- 蜂鸣器采用有上限的非阻塞脉冲，启动、停止、息屏和异常恢复路径都执行 fail-safe OFF；即使 I2C 写失败，也不能无限延长蜂鸣状态。

### 2.5 天气与其他管理界面

- 设备屏幕内只保留心知天气：启用、API Key、城市、15/30/60分钟更新和“更新天气”。
- NTP 未完成时显示“等待系统校时，联网后自动更新”，网络和时间就绪后自动重试；错误 Key、城市无效、额度限制、断网分别显示中文原因。
- 密钥不进入布局、草稿、导出、日志和状态 API；继续使用 HTTPS 证书校验。
- 外设配置的“终端/探测”保留，但统一收进“更多 → 诊断”，仅开发者模式且设备能力匹配时显示；普通用户主操作区不展示。
- 编辑外设执行将“执行后上报数据、保留最近10条记录、允许设备屏幕控制、配置最近记录”合并为一个“数据与屏幕”卡片，桌面两行两列，手机单列。
- 新生成的页面、组件和系统默认名称使用中文；内部 ID、枚举和用户已有自定义名称不改。自动生成的英文默认名可按类型映射为中文显示别名。

## 三、配置与兼容性约束

设备屏幕配置继续写入 `/config/screen_panel.json`，天气内部配置继续由现有天气管理器维护，外设与规则分别保持 `/config/peripherals.json` 和 `/config/periph_exec.json`。所有写入走 `ConfigStorage`；普通 GET、预览和刷新状态不得隐式写盘。

计划增加或规范化以下逻辑字段，实际名称以现有模型兼容规则为准并由测试固定：

```json
{
  "settings": {
    "homePageId": "home",
    "bottomNavPageIds": ["home", "trend", "motor", "control", "system"],
    "brightnessPercent": 70,
    "idleSleepSeconds": 120,
    "touchBeepEnabled": true,
    "controlConfirmIdleSeconds": 60
  }
}
```

- `bottomNavPageIds` 最多五项；缺失时按可见页面顺序推导，旧配置无需人工迁移。
- 显示 capability 增加/校准连续调光、最小可见亮度、背光极性、触摸唤醒和蜂鸣器能力；硬件参数属于显示外设配置，不复制进页面卡片。
- 旧的 `brightnessPercent=0` 读取时迁移为最小可见亮度；只有显式 sleep 状态关闭背光。
- 迁移函数幂等，保留有效自定义页面与控件，不按标题删除用户内容。
- 通用固件无显示、无触摸、无蜂鸣器、无电机或无外网时隐藏不可用控件并显示原因，不生成假能力。

## 四、实施任务

### Task 1：冻结现场基线并建立缺陷复现

**Files:**
- Modify: `test/browser/suites/31-screen-panel.spec.ts`
- Modify: `test/unit/test_touch_screen_interactions.py`
- Modify: `test/unit/test_screen_backlight.py`
- Modify: `test/unit/test_screen_system_actions.py`
- Modify: `test/unit/test_remote_weather_scheduler.py`
- Modify: `test/unit/test_motor_control_behavior.py`
- Reference: `docs/touch-screen-deployment-20260922.md`

- [ ] 记录设备 API 返回的固件版本、Web 资源版本、`screen_panel.json` revision、显示外设配置和功能 capability，先判断截图对应版本是否与当前源码一致。
- [ ] 增加失败测试：预览 Tab 不出现保存/导入操作；配置页只有一个主保存；底栏五项均可命中。
- [ ] 增加失败测试：数据卡/控制卡空白区域不执行，横向滑动不触发按钮，图表不抢占滑动。
- [ ] 增加背光真值表测试，覆盖 active-high、active-low、CH32、sleep、1/10/50/100%及恢复值。
- [ ] 增加蜂鸣器测试，覆盖启动默认关闭、普通点击短鸣、滑动不鸣、息屏不鸣、I2C 失败后超时关闭。
- [ ] 增加天气状态机测试，复现 NTP 阻塞后不会自动恢复或错误信息不友好的场景。
- [ ] 增加电机测试，按请求、队列和 Modbus 启动寄存器写入次数分别计数。
- [ ] 运行 `npm run test:host`，保存“预期失败项”清单；不能用修改断言的方式掩盖现场缺陷。

**退出条件：** 每个用户反馈都有稳定复现或被明确标为“仅实物可验证”，并区分源码缺陷、配置错误、硬件接线和部署版本不一致。

### Task 2：收敛屏幕配置模型与硬件能力契约

**Files:**
- Modify: `include/core/ScreenPanelManager.h`
- Modify: `src/core/ScreenPanelManager.cpp`
- Modify: `include/screen/ScreenBackend.h`
- Modify: `web-src/modules/runtime/screen-panel-model.js`
- Modify: `src/network/handlers/ScreenPanelRouteHandler.cpp`
- Test: `test/test_screen_panel_manager.cpp`
- Test: `test/unit/test_screen_panel_contract.py`
- Test: `test/unit/test_screen_panel_migration.py`

- [ ] 先写契约测试，固定五项底栏、系统设置、确认窗口和能力字段的默认值、范围及导入导出行为。
- [ ] 在 C++ 与 Web 规范化器中使用同一字段语义；未知字段保留或明确拒绝，不能两端静默产生不同布局。
- [ ] 将显示能力拆成 `brightnessContinuous`、`minVisibleBrightnessPercent`、`sleep`、`touchWake`、`buzzer`，缺失能力按安全默认处理。
- [ ] 固定主题、背景、默认首页、亮度和息屏设置从 Web 保存到快照再到实体渲染的完整数据链；后端不支持的外观能力不得显示为“已应用”。
- [ ] 新增旧亮度零值和旧页面导航的幂等迁移，不改变用户自定义页面 ID 与顺序。
- [ ] 对无屏、无触摸、无蜂鸣器和无电机目标返回明确 capability 与禁用原因。
- [ ] 运行 `npm run test:native` 与相关 Python 单元测试，确认模型、路由和迁移一致。

### Task 3：按 A 方案重构 Web“设备屏幕”页面

**Files:**
- Modify: `web-src/pages/screen-panel.html`
- Modify: `web-src/css/screen-panel.css`
- Modify: `web-src/modules/runtime/screen-panel.js`
- Modify: `web-src/modules/runtime/screen-panel-model.js`
- Test: `test/browser/suites/31-screen-panel.spec.ts`
- Test: `test/unit/test_screen_panel_web_runtime.py`
- Test: `test/unit/test_screen_panel_presentation.py`

- [ ] 先写桌面、手机和键盘交互测试，固定双 Tab 职责、三栏结构、抽屉、焦点顺序和唯一主保存入口。
- [ ] 预览 Tab 仅保留状态摘要、数据刷新、页面切换、屏幕画面和“编辑此页”；隐藏保存条、导入导出和真实控制。
- [ ] 配置 Tab 改为三栏：左栏页面与组件，中栏实时画布，右栏属性；页面重命名、排序、首页和删除收进页面菜单。
- [ ] 将心知天气、显示外观、触摸息屏和安全操作放进“全局设置”抽屉，默认只展开最常用项。
- [ ] 取消历史记录的独立“保存记录设置”，纳入“保存并应用”；天气刷新和立即息屏仍是清楚标注的即时操作。
- [ ] 将导入、导出、恢复默认放入“更多”，底部固定区只显示修改状态、校验摘要、撤销和“保存并应用”。
- [ ] 调整 `sp-body`、`sp-top-tabs` 和各区间距，保证 Tab 与卡片四周有一致 padding；减少截图中的大片无意义留白。
- [ ] 复用 `fb-card`、`fb-form`、`fb-btn`、`config-tabs`、`status-badge` 和 CSS 变量，不引入新 UI 框架。
- [ ] 手机端改为画布优先和属性抽屉，触控目标不小于 44×44 CSS px；桌面保持三栏。
- [ ] 运行 `npm run test:e2e:offline`，保存 1440 px 与 390 px 视觉截图并检查溢出、粘性保存条和 Tab padding。

### Task 4：实现实体屏幕五项底栏与统一手势仲裁

**Files:**
- Modify: `src/screen/ScreenRuntime.cpp`
- Modify: `include/screen/ScreenInteraction.h`
- Modify: `include/screen/ScreenGeometry.h`
- Modify: `web-src/modules/runtime/screen-panel.js`
- Test: `test/unit/test_touch_screen_interactions.py`
- Test: `test/unit/cpp/screen_geometry_harness.cpp`
- Test: `test/unit/test_screen_renderer_raster.py`

- [ ] 先写手势状态机测试：`Idle → Pressed → Dragging/Released/Cancelled`，验证一条手势只归属一个目标。
- [ ] 底栏使用配置中的最多五个页面，整块 72 px 高度参与命中；图标、短标题和选中态保持清晰。
- [ ] 五页以内直接跳转；超过五页时四个固定入口加“更多”，并保留左右滑动。
- [ ] 内容布局统一扣除底栏高度，禁止最后一行卡片被底栏遮挡。
- [ ] 去除普通卡片容器点击；仅页面入口和明确子控件进入动作命中表。
- [ ] 横移超过阈值立即取消按钮候选并转为翻页；在滑块上按下则锁定滑块，不触发翻页。
- [ ] 息屏唤醒使用独立吞噬状态，完整 release 后才恢复普通交互。
- [ ] Web 预览复用相同导航规则和底栏比例，但所有控制按钮为演示状态。
- [ ] 运行像素和交互 harness，验证 480×480、旋转和不同 stripe 高度结果一致。

### Task 5：简化趋势页并完善坐标信息

**Files:**
- Modify: `src/screen/ScreenRuntime.cpp`
- Modify: `web-src/modules/runtime/screen-panel.js`
- Modify: `web-src/modules/runtime/screen-panel-model.js`
- Test: `test/unit/test_screen_renderer_raster.py`
- Test: `test/unit/test_screen_panel_presentation.py`
- Test: `test/browser/suites/31-screen-panel.spec.ts`

- [ ] 先补 0/1/10 点、常值、负数、小数、不均匀采样、跨日和无 NTP 的绘制测试。
- [ ] 横轴根据真实采样时间定位，显示起点/中点/终点；纵轴显示约三档刻度及单位。
- [ ] 标题区显示当前值，图表下固定显示最新样本的“时间 · 数值”，不要求用户点击。
- [ ] 实体图表取消点选动作和点击热区，横向手势直接用于翻页。
- [ ] Web 预览可使用 hover 展示点详情，但 pointer down 不产生设备控制请求。
- [ ] 缺失样本不补零，断采区间断线；未校时时显示相对时间或样本序号。

### Task 6：统一电机交互并闭环“执行两次”问题

**Files:**
- Modify: `include/core/MotorControlService.h`
- Modify: `src/core/MotorControlService.cpp`
- Modify: `src/screen/ScreenRuntime.cpp`
- Modify: `web-src/modules/runtime/device-control/core.js`
- Modify: `web-src/modules/runtime/device-control/modbus-ctrl.js`
- Modify: `src/network/handlers/ModbusRouteHandler.cpp`
- Modify: `src/protocols/ModbusHandler.cpp`
- Modify: `src/core/PeriphExecManager.cpp`
- Test: `scripts/test-motor-control-lifecycle.js`
- Test: `test/unit/test_motor_control_behavior.py`
- Test: `test/helpers/motor_service_harness.cpp`
- Test: `test/helpers/motor_command_queue_harness.cpp`

- [ ] 先用生产事件绑定方法复现页面进入/退出20次后的一次点击，并断言只提交一个请求。
- [ ] 为 Web 大屏监听器使用可卸载命名处理器，销毁页面时解绑；同一 DOM 不重复绑定。
- [ ] 同一电机启动命令在途时禁用正/反转，停止保持可用并清理尚未执行的启动。
- [ ] 请求 ID 在前端、路由、服务、事件和结果中贯穿；相同 ID 重放只返回已有结果。
- [ ] 正转/反转和定量运动写不自动重试；响应丢失转为“状态待确认”并进行安全读回。
- [ ] 阻止 `motor-result` 或同源关联事件再次控制同一电机，保留跨设备合法联动。
- [ ] 实体电机页实现首次/超时确认、参数编辑与设置分离、停止免确认及错误中文提示。
- [ ] 对“电机状态不可用”显示具体分类：未绑定、总线离线、从站无响应、配置已变更、软限位、无反馈能力。
- [ ] Mock 总线验收以启动寄存器写入次数为准，速度/脉冲设置帧和状态读取帧不计作重复启动。

### Task 7：修复亮度方向、最低亮度与息屏恢复

**Files:**
- Modify: `include/screen/ScreenBacklight.h`
- Modify: `src/screen/RgbDisplayBackend.cpp`
- Modify: `src/screen/SpiTftDisplayBackend.cpp`
- Modify: `src/screen/ScreenRuntime.cpp`
- Modify: `include/core/PeripheralConfig.h`
- Test: `test/unit/test_screen_backlight.py`
- Test: `test/unit/cpp/screen_device_harness.cpp`
- Test: `test/unit/test_screen_system_actions.py`

- [ ] 先用真值表固定“1%最暗、100%最亮、sleep关闭、wake恢复”的逻辑值，不把 polarity 混入 UI 层。
- [ ] 分别核对 GPIO LEDC、CH32 PWM 和仅开关背光的实际电气语义；通过显示外设 profile 修正极性，不做全局强制取反。
- [ ] 连续调光后端把逻辑 1–100%映射到 capability 给出的最小可见 duty 至满量程；0%输入规范化到最低可见亮度。
- [ ] 仅开关背光的设备不显示连续滑块，只显示“亮/灭”能力和原因。
- [ ] 息屏保存最后可见亮度，唤醒恢复该值；配置保存与运行态 sleep 分离。
- [ ] 滑块最多约10次/秒更新硬件，松手后约2秒只保存最终值，避免 Flash 高频写入。
- [ ] 在目标 Waveshare 设备上实测 1/10/50/100%和息屏/唤醒，记录 PWM/寄存器值与肉眼结果。

### Task 8：把蜂鸣器改为可靠、可配置的触摸反馈

**Files:**
- Modify: `src/screen/ScreenRuntime.cpp`
- Modify: `src/screen/RgbDisplayBackend.cpp`
- Modify: `include/screen/ScreenBackend.h`
- Modify: `web-src/modules/runtime/screen-panel.js`
- Test: `test/unit/test_screen_system_actions.py`
- Test: `test/unit/test_touch_screen_interactions.py`
- Test: `test/unit/cpp/screen_system_actions_harness.cpp`

- [ ] 先测试启动、按钮、导航、滑动、唤醒、息屏、停止任务和 I2C 写失败下的蜂鸣器开关序列。
- [ ] 初始化控制扩展芯片时先写安全 OFF，再开放触摸事件；设备停止和异常退出再次强制 OFF。
- [ ] 提示音只在有效按钮或导航抬起后触发固定短脉冲；滑动、拖动、唤醒和立即息屏保持静音。
- [ ] 使用截止时间状态机关闭蜂鸣器，重复点击只延长到受限上限，不创建阻塞延时或无限重试。
- [ ] I2C 写失败时记录一次限频错误并继续 OFF 重试；成功关闭后清除 pending 状态。
- [ ] Web 只暴露“触摸提示音”开关和“测试提示音”即时按钮，不暴露无必要的频率/时长参数。
- [ ] 实机连续点击、息屏和重启各验证20次，任何情况下不得持续鸣叫。

### Task 9：修复心知天气可用性和状态反馈

**Files:**
- Modify: `include/protocols/RemoteResourceManager.h`
- Modify: `src/protocols/RemoteResourceManager.cpp`
- Modify: `include/protocols/RemoteResourceModel.h`
- Modify: `src/network/handlers/ScreenPanelRouteHandler.cpp`
- Modify: `web-src/modules/runtime/screen-panel.js`
- Test: `test/test_remote_resources.cpp`
- Test: `test/unit/test_remote_weather_scheduler.py`
- Test: `test/unit/test_remote_weather_preview_contract.py`

- [ ] 先覆盖未联网、等待 NTP、NTP 后恢复、错误 Key、城市无效、限额、超时、断网恢复和旧请求晚到。
- [ ] 天气启用且网络已就绪时主动请求系统校时；NTP 完成后唤醒天气 worker，不要求用户重复点保存。
- [ ] 将内部错误码映射为中文状态，并显示下一次自动重试时间；保留最后一次有效天气并标记陈旧。
- [ ] “更新天气”在配置未保存时先提示保存，不用旧 Key/城市发请求；相同配置保存不清空缓存。
- [ ] 保持 HTTPS、响应体8 KiB上限、超时、内存准入、OTA暂停和五分钟手动限频。
- [ ] 4G路径若仍不支持 TLS 请求，明确显示“当前网络通道不支持天气更新”，不笼统显示天气不可用。

### Task 10：精简外设配置和外设执行辅助界面

**Files:**
- Modify: `web-src/modules/runtime/peripherals.js`
- Modify: `web-src/modules/runtime/peripheral-io.js`
- Modify: `web-src/pages/fragments/peripheral-io.html`
- Modify: `web-src/pages/modals.html`
- Modify: `web-src/css/main.css`
- Test: `test/browser/suites/33-peripheral-list-diagnostics.spec.ts`
- Test: `test/browser/suites/06-periph-exec.spec.ts`
- Test: `test/browser/suites/05-peripheral.spec.ts`

- [ ] 先测试普通模式不显示终端/探测主按钮，开发者模式下仅兼容外设显示“更多 → 诊断”。
- [ ] 保留 UART 终端、I2C 探测、编码器读取等实际诊断能力，打开后明确说明用途、占用总线风险和关闭方式。
- [ ] 将外设执行三个复选框与“配置最近记录”合并为一个卡片：桌面2×2，手机单列。
- [ ] 保持规则编辑草稿；打开记录设置不保存、不关闭当前编辑内容。
- [ ] 新增外设/规则的系统默认显示名使用中文；已有用户名称和稳定 ID 不修改。
- [ ] 来源选择器优先展示“中文名称 · 类型 · ID”，避免 `Air temperature` 等内部英文标签直接成为主要文案。

### Task 11：保存协调、迁移和异常恢复

**Files:**
- Modify: `web-src/modules/runtime/screen-panel.js`
- Modify: `src/core/ScreenPanelManager.cpp`
- Modify: `src/network/handlers/ScreenPanelRouteHandler.cpp`
- Modify: `src/systems/ConfigStorage.cpp`
- Test: `test/unit/test_screen_panel_migration.py`
- Test: `test/unit/test_periph_exec_config_persistence.py`
- Test: `test/unit/test_peripheral_transaction_contract.py`

- [ ] 保存前统一验证布局、天气引用、历史来源、能力和权限，只提交脏分组。
- [ ] 天气/历史依赖成功后再保存布局；任一步失败都保留草稿并准确列出“已保存/未保存”分组。
- [ ] 每个配置接口继续执行 revision 校验，HTTP 409 时保留本地修改并提供重新加载/导出草稿。
- [ ] 旧导航、零亮度、空占位卡和英文系统默认名迁移保持幂等；不删除用户自定义卡片。
- [ ] 关键配置继续使用 `.tmp/.bak` 原子替换；预览 GET、天气状态和触控读取不得触发写盘。
- [ ] 导入导出过滤 API Key、会话信息及硬件敏感数据，导入前显示兼容性报告。

### Task 12：全量验证、实物验收与文档同步

**Files:**
- Modify: `docs/touch-screen-optimization-implementation.md`
- Modify: `D:\project\gitlab\FastBee-doc\src\device\system\screen-panel.md`
- Modify: relevant peripheral and PeriphExec guides under `D:\project\gitlab\FastBee-doc\src\device\`
- Update only existing test artifacts/reports required by repository conventions; do not create unrelated说明文件或临时脚本。

- [ ] 运行 `npm run test:host`，期望静态、契约和单元测试全部通过，仅允许仓库已记录的环境性跳过。
- [ ] 运行 `npm run test:native`，期望所有 Native C++ 测试通过。
- [ ] 运行 `npm run test:e2e:offline`，检查设备屏幕、外设、外设执行和设备大屏回归。
- [ ] 运行 `npm run test:web-assets`，总量、首屏、单资源、DOM和 gzip 预算不得通过抬高阈值解决。
- [ ] 优先构建 `esp32s3-F16R8`、`esp32s3-F8R8`，随后按现有脚本完成九环境矩阵；无屏目标必须保持屏幕功能裁剪。
- [ ] 部署前核对固件、LittleFS Web 资源和配置 schema 版本一致，避免“源码已修复、设备仍运行旧资源”。
- [ ] 实物验证底栏五项、滑动、按钮命中、趋势显示、天气恢复、电机单次启动、亮度方向、最低亮度、息屏唤醒和蜂鸣器。
- [ ] 记录连续触控、电机和天气并行运行的内存走势；短测通过后再执行24/72小时稳定性测试。
- [ ] 文档明确区分“本地测试通过”“已部署”“实物功能通过”“长期测试通过”，不得用模拟结果代替硬件结果。
- [ ] 未得到用户明确授权前不提交 Git、不烧录、不操作真实电机。

## 五、验收矩阵

| 编号 | 验收项 | 通过标准 |
| --- | --- | --- |
| A01 | Web 信息架构 | 预览无保存工具；配置为三栏；只有一个主保存；Tab 与卡片边缘留白一致 |
| A02 | 五项底栏 | 五页都能一次点击直达；热区≥44 px；当前页清晰；首页始终可达 |
| A03 | 手势 | 卡片空白不执行；左右滑动不误点；滑块不翻页；唤醒首触不穿透 |
| A04 | 趋势 | 显示真实时间轴、数值轴、单位和最新值；实体图表点击不妨碍滑动 |
| A05 | 电机 | 正转/反转/停止直观；首次和闲置后确认；一次点击仅一次启动写；停止优先 |
| A06 | 电机状态 | 未绑定、离线、无反馈、超时、限位分别显示；不把命令接受当作真实运行 |
| A07 | 显示与亮度 | 主题/背景保存后实体生效；100%最亮，1%/最低档仍可见，0不导致不可恢复黑屏；唤醒恢复原亮度 |
| A08 | 息屏 | 独立按钮；首次触摸只唤醒；无触摸能力时禁止危险息屏 |
| A09 | 提示音 | 开关可配；按钮/导航短鸣；滑动、唤醒、息屏不鸣；绝不持续鸣叫 |
| A10 | 天气 | Key与城市即可配置；NTP后自动恢复；异常中文可诊断；无密钥泄漏 |
| A11 | 外设诊断 | 普通界面简洁；高级诊断仍可用且不会误占 Modbus 等共享总线 |
| A12 | 外设执行 | 三个复选框加一个按钮在2×2卡片内；草稿和权限语义保持 |
| A13 | 中文化 | 新增默认名称中文；稳定 ID 和用户自定义名称不变；内部英文不作为主标签 |
| A14 | 通用性 | 无对应能力时隐藏/禁用并说明；不同显示后端不共享错误极性假设 |
| A15 | 兼容性 | 旧配置幂等迁移；有效自定义页面不丢失；回退路径明确 |
| A16 | 资源与稳定性 | Web预算、九目标构建、Host/Native/E2E通过；实物短测与24/72小时结果分开记录 |

## 六、实施顺序、风险与回退

推荐顺序为：Task 1基线 → Task 2契约 → Task 7/8硬件安全缺陷 → Task 4/5触控交互 → Task 6电机 → Task 9天气 → Task 3 Web重构 → Task 10辅助界面 → Task 11迁移 → Task 12全量验收。亮度反向、黑屏、蜂鸣器持续和电机重复动作属于硬件安全/可恢复性问题，优先于视觉润色。

主要风险：

- 当前工作区代码与设备已部署固件/Web资源可能不一致，必须先做版本指纹核对。
- CH32、GPIO LEDC 和不同屏幕背光极性不同，不能用一个全局反转修复所有设备。
- 电机协议若没有幂等命令和运行反馈，只能保证固件不主动重复提交，不能承诺断电级 exactly-once。
- 三栏编辑器可能增加 DOM 和资源体积，必须在现有质量门禁内调整，不提高预算掩盖问题。
- 天气依赖网络、NTP、TLS和服务额度；UI需准确区分状态，不能把外部条件都显示成“不可用”。

回退策略：保留现有配置 revision、迁移前备份和旧字段读取；固件与 Web 资源作为同一版本发布。若实体屏幕新交互失败，可回退到上一套渲染行为，但不得恢复亮度反向、持续蜂鸣或重复启动等安全缺陷。

## 七、本轮明确不做

- 不引入 LVGL、React、Vue、远程图表库或第二套屏幕渲染引擎。
- 不把通用固件绑定到单一 Waveshare 型号；该设备只作为首个实物验收目标。
- 不恢复 MQTT 远程通道、自定义天气、预报、通用鉴权和字段映射。
- 不把普通继电器伪装成电机，不允许页面配置任意 Modbus 寄存器。
- 不新增与任务无关的 Markdown、样例、临时脚本或重复配置文件。
- 不在计划审核阶段修改业务代码、烧录设备或操作电机。
