# FastBee-Arduino 重构优化计划

## 1. 文档目的

本计划用于指导 FastBee-Arduino 项目的前端与交互层重构，目标是同时改善以下几个方向：

- 提升页面加载与运行性能
- 改善配置流程和细节体验
- 减少重复代码和分散实现
- 减少行内样式，优先复用已有样式体系
- 控制发布体积，降低 ESP32 静态资源压力
- 在不破坏现有功能的前提下逐步推进重构

本计划基于当前仓库结构、前端资源体积、模块加载方式和页面实现现状整理，尽量贴近项目实际，不采用脱离 ESP32 场景的“纯 Web 项目式重构方案”。

## 2. 当前现状概览

### 2.1 当前前端结构

- 前端静态资源位于 `data/www/`
- 单页主入口为 `data/www/index.html`
- 主样式文件为 `data/www/css/main.css`
- 基础状态与模块加载逻辑集中在 `data/www/js/state.js`
- 页面模块位于 `data/www/js/modules/`
- 资源压缩脚本位于 `scripts/gzip-www.js`

### 2.2 代码层面的已知优势

- `main.css` 已经具备较完整的设计变量体系，包含颜色、间距、圆角、阴影、主题变量，说明项目已经有“统一样式系统”的基础
- `state.js` 已具备动态模块加载、全局事件委托、bundle 映射和有限并发加载机制，可作为后续收敛交互实现的基础设施
- 项目已使用 `.gz` 资源发布，对 ESP32 的静态资源传输是有帮助的

### 2.3 当前暴露出的主要问题

#### 页面和资源体积偏大

- `data/www/index.html` 约 `271 KB`
- `data/www/css/main.css` 约 `114 KB`
- `data/www/js/state.js` 约 `55 KB`
- `data/www/js/modules/protocol.js` 约 `117 KB`
- `data/www/js/modules/periph-exec.js` 约 `76 KB`
- `data/www/js/modules/admin-bundle.js` 约 `71 KB`
- 当前前端核心资源总量约 `1,150,813 bytes`

#### HTML 结构过重

- `index.html` 中包含 `13` 个页面区域
- `index.html` 中包含 `12` 个模态框
- `index.html` 中存在 `187` 个 `style=` 行内样式
- `index.html` 中有 `2` 处 base64 图片直接内嵌，主入口体积被进一步放大

#### 样式复用不足

- 项目已经有设计 token 和 `fb-*` 风格工具类，但页面仍大量使用行内样式
- 多处重复出现相同布局和视觉片段，例如：
- `padding: 10px 15px`
- `display:flex; align-items:center; gap:...`
- 状态提示块、卡片头、信息面板、空状态表格、文件管理面板、进度条外层/内层

#### JS 模块中存在重复模式

- 多个模块重复处理：
- modal 打开/关闭
- 错误提示显示/隐藏
- 表格空状态渲染
- `innerHTML` 拼接按钮、表格行、标签
- `style.display` / `style.color` / `style.marginRight` 等直接操作

#### 存在可疑的发布冗余

- `state.js` 中 `_bundleMap` 已将 `users`、`roles`、`files`、`logs`、`rule-script` 映射到 `admin-bundle`
- 但发布目录中仍保留 `users.js`、`roles.js`、`files.js`、`logs.js`、`rule-script.js`
- 这几份文件合计约 `71,418 bytes`
- 基于当前入口逻辑判断，这部分很可能属于“发布目录内的冗余副本”或“源码与产物未分层”

说明：
这里的“可疑冗余”是基于当前入口加载逻辑做出的工程判断，真正删除前仍需做一次引用清点和回归测试。

## 3. 重构原则

### 3.1 以 ESP32 场景为前提

- 不引入重型前端框架
- 不盲目把页面拆成大量运行时 HTML 请求
- 不破坏当前 gzip 发布流程
- 任何优化都要兼顾“资源体积、连接并发、浏览器兼容、设备侧吞吐”

### 3.2 优先复用已有基础能力

- 复用 `main.css` 中现有设计 token
- 复用 `state.js` 中现有事件委托与模块加载机制
- 复用已有按钮、卡片、表单、消息、弹窗等样式能力

### 3.3 先收敛，再美化

- 第一优先级是减少重复实现和风险点
- 第二优先级是减体积和提性能
- 第三优先级是视觉与交互细节统一

### 3.4 避免一次性大改

- 使用分阶段改造
- 每个阶段都有明确的验收指标
- 每个阶段都应保证可独立回退

## 4. 重构目标

### 4.1 性能目标

- 主入口 HTML 体积下降 `20%~35%`
- 行内样式数量从 `187` 降到 `30` 以内
- 重复发布模块从发布目录中剥离或合并
- 页面切换过程减少无意义 DOM 重绘和字符串拼接

### 4.2 维护性目标

- 公共 modal、table、badge、panel、empty-state 统一组件化
- 公共 DOM 操作和 UI 状态切换收敛到共享工具层
- 禁止新增大段行内样式和无约束的 `innerHTML` 拼接

### 4.3 用户体验目标

- 加载、空状态、错误状态、保存成功提示统一
- 表单分组、按钮层级、状态色、间距节奏统一
- 模态框行为一致
- 页面在桌面端和窄屏下都更稳定

## 5. 分阶段实施计划

## 阶段 A：建立基线与约束

### 目标

在真正重构前先把“哪些页面大、哪些模块重复、哪些交互最容易坏”固定下来，避免后续改完没有量化结果。

### 工作项

- 建立前端资源体积基线表
- 建立页面回归清单：
- 登录
- 仪表盘
- 设备配置
- 网络配置
- 外设配置
- 外设执行
- 设备控制
- 协议配置
- 规则脚本
- 文件管理
- 日志
- 用户和角色
- 建立样式约束：
- 新代码默认禁止新增行内样式
- 动态尺寸仅允许通过 CSS 变量或极少量运行时 style 设置
- 建立模板约束：
- 禁止新增大段拼接式 `innerHTML`
- 优先使用模板函数、DOM 构建辅助或统一渲染器

### 产出

- 一份资源与页面基线表
- 一份 UI 约束清单
- 一份回归检查清单

## 阶段 B：样式系统收敛与去行内样式

### 目标

把当前页面中最明显的视觉重复与样式散落问题收敛到统一 class 和 utility 层。

### 工作项

- 在 `main.css` 中补齐高频公共类
- 优先抽取以下模式：
- `.panel-header`
- `.panel-body`
- `.info-row`
- `.inline-meta`
- `.stack-gap-*`
- `.flex-row`
- `.flex-wrap`
- `.status-badge`
- `.muted-text`
- `.mono-text`
- `.empty-row`
- `.progress-shell`
- `.progress-bar`
- `.section-note`
- `.danger-note`
- `.warning-note`
- `.info-note`
- 用 class 替换 `index.html` 中的重复 `style=`
- 把文件管理、OTA、BLE、NTP、系统操作区域中重复的“卡片头 + 内容体”样式统一
- 把各种状态色块、说明块、提示块统一为语义 class
- 进度条宽度改为：
- 保留少量运行时 `style.width`
- 或改为 CSS 变量驱动，如 `--progress: 35%`
- 将 base64 logo 替换为统一图片资源引用，避免主入口重复嵌入

### 验收指标

- `index.html` 行内样式减少到 `80` 以下
- `main.css` 中新增的公共类能覆盖主要重复块
- 页面视觉不回退
- 深浅主题表现一致

## 阶段 C：公共交互能力抽象

### 目标

把各模块中重复的 modal、表格、按钮状态、错误提示、状态标签逻辑收敛成公共实现。

### 工作项

- 在 `state.js` 或独立公共模块中抽取：
- `showModal(id)`
- `hideModal(id)`
- `toggleVisible(el, show)`
- `renderEmptyRow(tbody, colspan, text)`
- `setError(el, text)`
- `clearError(el)`
- `setLoading(btn, text)`
- `restoreButton(btn, text)`
- `renderBadge(type, text)`
- 清理模块内重复逻辑，优先处理：
- `users.js`
- `roles.js`
- `rule-script.js`
- `files.js`
- `logs.js`
- `admin-bundle.js`
- 把“表格无数据提示”的内联字符串统一
- 把“打开 modal 后清空错误 / 关闭 modal / 保存后恢复按钮文案”的重复流程统一
- 优先复用当前 `state.js` 中已有的全局事件委托能力，减少临时 `onclick` HTML

### 验收指标

- modal 相关公共逻辑统一到一处
- 至少 5 个模块去掉重复的错误显示/关闭弹窗逻辑
- 相同交互的按钮、错误提示、空状态表现一致

## 阶段 D：模块去重与发布体积优化

### 目标

减少“发布目录里存在多份相似脚本”的问题，让源码组织和发布产物分层清晰。

### 工作项

- 明确 `admin-bundle` 的定位：
- 如果它是正式发布 bundle，则将 `users.js`、`roles.js`、`files.js`、`logs.js`、`rule-script.js` 迁出发布目录
- 如果这些文件仍要保留开发态源码，则应迁移到独立源码目录，不直接放在 `data/www/js/modules/`
- 为前端建立最小化构建流程：
- 开发态源码目录
- 发布态输出目录
- gzip 前执行 minify / tree-shaking / 文件清点
- 梳理 i18n 资源：
- 明确 `i18n.js` 与 `i18n-en.js` 的职责
- 如果存在重复语言数据，改成主包 + 语言包增量结构
- 按页面维度拆分超大模块：
- `protocol.js`
- `periph-exec.js`
- `device-control.js`
- 但拆分策略必须以“运行时请求数可控”为前提
- 建议采用“构建期拆分 + 按需 bundle”，而不是“运行时大量拉散文件”

### 验收指标

- 发布目录不再保留未被入口使用的冗余模块
- 发布资源总量下降
- gzip 前后的产物关系清晰可追踪
- `scripts/gzip-www.js` 可以接入构建后的统一输出目录

## 阶段 E：页面结构与模板治理

### 目标

降低 `index.html` 的维护成本，但不把运行时请求拆得过碎。

### 工作项

- 保持单页入口思路不变
- 将大型重复区块改成可维护模板：
- 原生 `<template>`
- JS 模板函数
- 构建期片段拼装
- 优先治理以下区域：
- 重复面板头结构
- 各类状态信息卡
- 设备信息表格
- OTA 与系统操作区
- 文件管理双栏布局
- 各类 modal 标题和 footer
- 将页面级别的复杂区域从静态 HTML 挪到对应模块内渲染，但避免拆成运行时远程模板

### 验收指标

- `index.html` 体积明显下降
- 页面结构更聚焦，按功能分区更清晰
- 模块负责自己的模板与交互，不再把大量静态结构堆在单个 HTML 中

## 阶段 F：UI 细节与体验升级

### 目标

在结构收敛后统一细节体验，提升专业感与可用性。

### 工作项

- 统一空状态、加载状态、错误状态文案与视觉
- 统一表格头、筛选栏、分页、操作按钮间距
- 统一 modal 行为：
- 打开时聚焦首个输入框
- ESC 关闭
- 点击遮罩关闭策略统一
- 保存中按钮禁用与文案统一
- 优化表单：
- 必填项提示统一
- 错误信息位置统一
- 成功提示不遮挡主操作区域
- 优化窄屏体验：
- 文件管理区、卡片双栏区、筛选栏在小屏下自动折行
- 优化可读性：
- 单位、说明、小字、状态标签层级更清晰
- 优化主题一致性：
- 深色模式下对比度、边框、状态色更稳定

### 验收指标

- 主要配置流程的点击路径更顺畅
- 窄屏下不出现大面积遮挡、挤压、超宽滚动
- 所有状态型反馈都有统一视觉语言

## 6. 优先改造文件建议

### 第一批

- `data/www/index.html`
- `data/www/css/main.css`
- `data/www/js/state.js`
- `data/www/js/modules/admin-bundle.js`
- `data/www/js/modules/protocol.js`

### 第二批

- `data/www/js/modules/periph-exec.js`
- `data/www/js/modules/device-control.js`
- `data/www/js/modules/network.js`
- `data/www/js/modules/device-config.js`

### 第三批

- `scripts/gzip-www.js`
- 前端构建与发布脚本
- i18n 资源组织方式

## 7. 建议的落地顺序

### 第一轮：低风险高收益

- 去掉最明显的行内样式
- 抽公共 class
- 抽 modal / empty row / error 辅助
- 清理 base64 logo

### 第二轮：中等风险高收益

- 收敛 admin 相关重复逻辑
- 整理 `protocol.js` 和 `periph-exec.js` 的渲染与事件处理
- 清理未使用或重复发布的脚本

### 第三轮：结构优化

- 重做发布目录与源码目录边界
- 补构建脚本
- 进一步缩减 `index.html`

## 8. 风险与控制措施

### 风险点

- 页面较多，单次大改容易造成隐藏回归
- ESP32 静态资源请求能力有限，不能套用普通前端项目的“细粒度拆包”方案
- 当前前端已存在用户本地修改，批量清理样式和结构时容易产生冲突

### 控制措施

- 每次只改一个主题域：
- 样式域
- modal 域
- 表格域
- 某个页面域
- 每轮改造后都做页面回归清单检查
- 先保留 DOM 结构，再替换 class 和交互实现
- 构建优化放在样式和交互收敛之后进行

## 9. 建议的验收指标

### 代码指标

- `index.html` 行内样式数量
- 模块中 `innerHTML` 大段拼接数量
- 模块中直接 `style.*` 操作数量
- 发布目录实际被入口使用的模块数量

### 体积指标

- `index.html`
- `main.css`
- `state.js`
- `protocol.js`
- `periph-exec.js`
- gzip 前总量
- gzip 后总量

### 体验指标

- 登录耗时感知
- 页面切换稳定性
- 表单保存反馈一致性
- 窄屏可用性
- 深浅主题一致性

## 10. 最终建议

这个项目不适合“推倒重来”，最合适的是做“渐进式重构”：

- 保留当前单页架构和 ESP32 友好的加载思路
- 先把样式和交互公共层收敛
- 再做模块去重和发布体积优化
- 最后处理 UI 细节和构建流程

从投入产出比来看，优先级最高的工作是：

- 清理 `index.html` 行内样式
- 抽公共 modal / panel / empty state / badge
- 整理 `admin-bundle` 与独立模块的发布关系
- 拆解 `protocol.js` 与 `periph-exec.js` 的重复渲染和内联 HTML

如果按这个顺序推进，能够在不明显增加风险的前提下，同时拿到性能、维护性、项目体积和用户体验四个方向的收益。
