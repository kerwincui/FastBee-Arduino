# G05/G06：屏幕草稿在刷新前同步保留

2026-10-02。本批原在 `artifacts/system-plan-screen-draft-stage-20261002-r1` 独立准备，现经 `artifacts/system-plan-development-promotion-20261002-r3` 合入主目录，**当前主目录 UNTESTED，不属于 r14**。r15 已取消，待开发完成集中测试；没有设备访问、共享 Web 发布或现场配置写入。

## 失败与根因

r14 的 Host contracts 离线矩阵为 184 PASS/1 FAIL：WEB-PANEL-034 在设备设置 revision 变化后恢复草稿，标题退回默认值。原始报告、截图和 trace 均保留在隔离 r14 的 Web configuration 输出中，不以复跑结果覆盖。

编辑器将草稿写入 localStorage 延迟 160ms。已有键可能只保存了前一次编辑；测试等待键存在不能证明最新标题已写入。浏览器刷新会销毁尚未执行的定时器，原模块没有 browser pagehide 同步刷新；SPA 页面隐藏也只重新排队延迟保存。

## 修复

- 保存仍使用原来的 160ms 去抖，保存回调记录在 model 中；浏览器 pagehide 和页面隐藏时同步完成待保存草稿，不等待定时器。
- 继续使用排队时绑定的 MAC；MAC 不明、切换设备、清除草稿时取消待保存操作，不能把 A 设备草稿写成 B 设备。
- 同时保留布局与 history 草稿；revision/基线、三方合并及冲突处理规则不变。存储空间不足或禁用仍捕获错误，不阻止设备保存。
- 不添加网络请求、不保存设备配置、不执行卡片动作。浏览器崩溃、操作系统强杀、用户清理存储等没有 pagehide 的情况不保证最后一笔草稿落盘。

## 已完成验证

- 原确定性 Node 场景 1 FAIL/1 PASS（before-r1.log）；修复后 2 项分别在源码与 Terser 压缩版执行，全部通过（after-r2.log，12.184s）。覆盖最新内容/原 revision 基线/history、MAC 缺失与切换、存储异常和定时器清理。
- 独立 loopback 浏览器 23/23 PASS（browser-r1.log，2.7m），含 r14 失败的跨 revision 恢复、并发设置合并、真实冲突保留、18 类卡片和部分保存/响应式场景。
- 新增 WEB-PANEL-037 把编辑器的 160ms 定时器确定性延后，立即刷新再恢复：旧发布资源 FAIL，原 browser-before-r2.log、截图/trace 保留；修复后 PASS（browser-after-r2.log，14.1s）。旧日志中该新用例临时使用 035，与既有恢复测试重号；已改为 037，测试断言不变，原日志不重写。
- local-validation.json 记录 24 次浏览器通过、2 项双版本 Node 测试、源码及日志哈希。浏览器使用已有冻结 Web 副本并只路由准备区屏幕源码，不是新发布产物的验收；完整 Web 预算、全 Host 和设备验证仍留给后续候选。

## 接续

主目录推广已完成，证据在 `artifacts/system-plan-development-promotion-20261002-r3/applied.json`。r15 已取消；旧 artifacts/prepare-system-plan-gt911-draft-promotion-20261002.py 不能再执行。合入后源码和完整 Web 发布尚未复验，按最新要求等开发完成再集中测试。
