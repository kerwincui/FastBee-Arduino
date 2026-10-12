# 普通原生数字输出的 HTTP / Screen 消费入口

2026-10-04 后继软件候选已落源码，新增测试尚未运行，等待主线程串行集中验证。r4 原失败和归档结果不覆盖本次修改；设备、继电器触点和负载反馈均为 NOT_RUN。

## 请求归属与范围

`HttpOutputControl` 将普通原生非被动 GPIO_DIGITAL_OUTPUT 的手动 high/low/toggle/blink/stop 交给 PM 的统一普通输出仲裁。HTTP actor 只取认证会话的服务端 `outputActor`，不得从客户端传入 source/actor/operation。请求只允许明确列出的 POST 字段，拒绝查询串、文件、重复键、未知键与冲突命令；布尔电平限定 0/1，blink 间隔 1～65535ms，手动占用默认30秒、明确请求为1～300000ms。

此路径不将 DAC、PWM、servo、IO 扩展器或专用 ControlOutput 纳入普通 GPIO 权限。普通输出不能覆盖专用温控/RESET/OE claim。高低电平含义及旧 blink 定时器的终止/所有权由实际 PM 仲裁实现负责，不由 HTTP 重演电平。

后继候选已将PWM/AO/DAC及passive波形接到独立typed scalar入口、复用同一普通来源租约，具体范围见[原生PWM与passive接续](system-improvement-passive-pwm-control-20261004.md)。这不把它们当数字GPIO写，也不让局部接口或夹具通过冒充整套门禁/实物通过。IOX、NeoPixel、PCA及servo/stepper另按各自集成证据验收。

手动规则仍经原权限检查与 `runOnce`，以 Http actor、原配置 epoch 和无接管确认的上下文入队。单条规则可能涉及多个目标，不能借一个目标的确认覆盖全部输出。规则队列准入后由 manager 完成上下文；拒绝入队则边界取消。直接单目标请求提交后总是正常 finish，SDK 失败也不重放、更不把它改写成无副作用的冲突。

## 显式接管与失败响应

直接请求只在 `confirmOverride=true` 时接收完整 review：原目标ID、十进制 uint64 generation、初次目标快照的 config epoch。确认必须与当前目标/配置一致；过期、部分或无确认附带 review 均拒绝。PM 最终复核仍是 authority，HTTP 不在途中重新捕获新版本来使旧操作通过。

结构化 control 响应包含 targetId、reason、evaluated、source、phase、holdRemainingMs、attempted/accepted/cleanupConfirmed；operation/generation 输出十进制字符串，防止 JavaScript 丢失64位精度。未进入原生仲裁表示 CONTROL_NOT_ATTEMPTED，不把它误报为实际拒绝。只有未尝试写入、Rule/Mqtt owner 的 Held/Producing 冲突可提出目标级确认；Http/Screen冲突与输出未知均不能提示自动接管。`physicalVerified=false` 始终明确，本机接受不证明负载动作。

普通拒绝返回409；发生过实际写尝试但结果未知返回503。run结果与近期执行记录复用同一 serializer；分配失败不得伪造成功响应。结果未知、网络失败和确认失效都不自动重新发送输出。

## 屏幕身份与锁顺序

Screen actor 来自已保存布局 revision 和跨全部保存页面的1基卡片序号。实际触摸入口携带显示时 revision 与 controlRef；保存状态、绑定、规则指纹或版本不一致时拒绝。revision0/MAX 不成为有效身份，成功持久化/加载后才通过 `ScreenOutputIdentity` 发布新版本。保存失败、初始化失败和失败迁移不会发布未提交布局。

PanelLock 不得跨 PM 或 runOnce：PM 生命周期回调会反向取得 PanelLock，同步回退执行也会进入 PM。dispatch 在 PanelLock 内检查后释放，再提交带版本 actor；PM 的 begin/submit/producer/finish 复核发布版本。入队成功后若结果映射锁忙/布局变化，返回已提交并保留operation，不把无法更新UI缓存当作输出失败诱导重试。屏幕终态缓存保留首次仲裁原因（例如 CONTROL_CONFLICT），不退化为 action_failed。

## 网页结果

两处手动规则入口复用实际共享 helper。快速重复点击同一规则在请求/有限观察期间只提交一次，最多在400/800/1200ms各发一次只读结果查询；离页/生命周期变化立即停止消费晚到结果。第三次结果仍必须显示成功、拒绝或尚未完成。空记录、错operation、网络/解析失败保持结果未确认；不重新POST，不自动确认多目标接管。

## 待执行回归

- `test_http_output_control.py`：完整实际HTTP helper/serializer，边界仅替换会话与PM；覆盖伪造字段、命令/范围、hold、确认、64位generation、配置epoch、冲突来源与未知结果。
- `test_manual_output_web.py`：实际utils/periph-exec/device-control模块；覆盖三次查询终态、待定、失败、记录不匹配、离页、快速双击与零自动重放。
- `test_screen_panel_contract.py`：实际dispatch方法；覆盖保存序号、显示版本、绑定变化、检查途中改版、无持锁调用PM/exec、拒绝取消、提交后锁忙及无回绕。
- `test_screen_config_persistence.py`：实际initialize/save/write方法；覆盖失败初始化、不持久默认、失败迁移、失败保存、旧actor失效及MAX。
- `test_screen_snapshot_cache.py`、`test_panel_runtime_regressions.py` 与Native完整Screen TU：缓存原拒绝原因及显示版本调用集成；原有motor路径保留。

这些测试不是实际FreeRTOS抢占、认证端到端、闪存时延、电气输出或长期验收。统一Host/Native/Web/目标构建和主线程核心仲裁回归均须有新的结果；G00～G06/H1、全动作字段共源、设备矩阵、资源/内存与24h/7d/V14不能凭本增量关闭。
