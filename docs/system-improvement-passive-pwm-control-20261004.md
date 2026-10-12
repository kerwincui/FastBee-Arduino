# 原生 PWM、AO 与无源蜂鸣器的来源仲裁及波形接续

状态：2026-10-04 后继软件候选；本说明对应新增源码/行为夹具，尚未经过集中测试或构建，未部署。实物、声响、负载与长期运行仍为 NOT_RUN。

## 保留的动作能力

`PeripheralActionContract` 原本就允许 PWM blink/toggle 和 AO toggle。首批 scalar 路由遗漏这两条，后继候选补回同一普通来源路径，不回退到无上下文的旧写接口。目录谓词未放宽，新增目录断言与真实 PM 行为夹具一一对应。

| 原生目标 | 高/低、反向高/低 | PWM 数值 | toggle | blink | breathe |
| --- | --- | --- | --- | --- | --- |
| PWM 输出 | 最大值/0 | 0～(2^分辨率−1)，AO模式为0～100% | 有 | 有 | 有 |
| AO 输出 | 配置最大/最小 | AO模式为0～100% | 有 | 拒绝，目录不宣称支持 | 有 |
| 数字输出且 passive=true | 一半最大占空比/0 | 原始占空比 | 有 | 有 | 拒绝，目录不宣称支持 |
| DAC | 仍由 DAC 值路径处理 | 非 PWM 路径 | 拒绝 | 拒绝 | 拒绝 |

8 位 passive HIGH 为127，保持既有整数半占空比语义；普通 PWM HIGH 为255。passive 使用 setupHardware 已有的默认2700Hz/8位配置，不把满占空比当成发声证据。原始 PWM 数值不自动变成布尔值。AO 的逻辑 LOW/0%仍映射配置最小值，可能有非零电压/电流。

## 来源、生产者与失败

Typed、CALL、命名脚本、HTTP 与 MQTT 复用现有真实 actor、operation、配置 epoch 和 UART/rule/screen 来源。未引入默认actor、第二租约或独立调度器；servo/stepper、协议输出由各自候选负责。

波形复用 `ActionTickerData` 与受检 `CheckedActionTimer`。blink 使用模式3、1～65535ms间隔；breathe继续模式2/20ms节拍。候选内存/节点先分配，旧生产者受检退休成功后才发布新 permit。过时来源、过期hold和旧数字token不再写入新目标。

HIGH/LOW/PWM成功的SDK应答更新最后逻辑请求电平。toggle不读取引脚推测PWM占空比，只翻转这个已确认的软件状态；状态未知直接拒绝。blink每次写失败令逻辑状态未知并尝试受检停表；历史错误不清零，不自动重复失败写入。停表/删除失败保留计时器、owner与Fault，阻止新输出，后续维护只重试清理。成功停表只证明没有本机排队写入，不证明输出已经关闭。

Stop、过期和来源退休均保持最后电气输出，不额外写0。旧无标签 Ticker 缺少受检停止证据，回调仅被抑制、记录被保留；不能以void detach冒充确认退休。其配置修改仍被锁定，需要独立确认重启/迁移。

HTTP继续服务端会话身份、严格POST字段、单动作、hold及完整review检查，未知结果不自动重放。scalar新增toggle、blink/intervalMs字段；混用periodMs/intervalMs、AO blink、passive breathe、DAC toggle均在边界拒绝。MQTT passive保留原布尔0/1语义，成功回显0/1；它不会把任意数字当成新的蜂鸣器占空比命令。

## 待集中验证

新增22组真实普通 PM/consumer 波形用例与4组实际HTTP边界用例，补目录组合断言、实际脚本解析范围及四芯片DAC/脚本夹具的真实类型依赖。覆盖半占空比、raw PWM、已知/未知toggle、AO最小值、blink替换、失效UART/过期hold、起停失败、晚到回调、无上下文与旧生产者拒绝。

r4 UART夹具另行修正其显示/PWM误报成DO扩展通道的问题，原失败日志保留；它的通过不能替代本候选验证。统一Host/Native/Web与目标构建、设备及24h/7d/V14仍须独立验收。
