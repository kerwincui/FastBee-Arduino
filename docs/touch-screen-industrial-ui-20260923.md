# 触控屏工业操作界面改进

本轮依据用户实际触屏反馈实施，覆盖界面、操作确认、声音、背光和时间同步。与前一轮部署记录分开保存；本轮状态以文末验证及部署记录为准。

## 界面与操作约定

- 固定底栏保留首页、当前页名称和页码，新增明确的前后页按钮；滑动仍可翻页。触控和绘制共用矩形，按钮间留出空隙。
- 使用简洁的深蓝灰卡片和清晰的值/操作分组。电机速度、脉冲各一行，减/加/应用在同一行；正转、反转并排，停止使用下一行宽按钮。没有运动反馈时明确显示“运行状态未确认”。
- 正反转首次操作先确认，成功提交后同一电机可连续操作；默认闲置 60 秒后重新确认，可设 15–600 秒。换绑定、改配置、息屏和重启均清除会话。参数应用不启动电机，停止不需要确认，首次确认弹窗中也提供停止。
- 电机提交携带用户看到的绑定，后台与当前配置核对后才接受，防止 Web 同时换绑造成旧画面操作新电机。
- 系统默认集中显示亮度、自动息屏、触摸提示音、操作确认间隔、立即息屏。默认不放蜂鸣测试和重启；原有显式自定义功能保持兼容。
- 息屏后触摸任意位置唤醒，第一次触摸直到松手只用于唤醒。驱动失败时保留息屏状态，不假称已经唤醒。

布局参考 [Siemens 固定主导航](https://docs.tia.siemens.cloud/r/en-us/v3.0/automation-framework/automation-framework-v3.0/3.-overview/3.5.-hmi-software-architecture/3.5.4.-navigation?contentId=_AfI88xffTmrmsC~cnMNvQ)及 [Rockwell HMI Style Guide](https://literature.rockwellautomation.com/idc/groups/literature/documents/wp/proces-wp023_-en-p.pdf)中的触摸目标、命令间隔和中性色彩原则，按现有 480 × 480 设备与轻量条带渲染器实现；不宣称符合完整工业 HMI 标准。

## 背光与声音

已定位 CH32 背光分支忽略 `backlightActiveHigh`，导致这台设备的亮度方向与实际电路不匹配。所有背光路径统一应用极性，普通 0–100 亮度映射为约 10%–100% 电气输出；显式息屏独立关闭背光，醒后恢复最后亮度。该电气映射不等于测量过的光学亮度。

现场显示外设需要将 `backlightActiveHigh` 由 true 调整为 false。该值根据这台设备的用户实测配置，不全局推断所有 Waveshare 板次都是低有效。厂商当前示例说明与本机反馈存在差异，见[官方 IO 扩展说明](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4/blob/main/examples/esp-idf/ioexpander/README.md)。

提示音为通用触摸反馈，可在屏幕设置开启/关闭；显示外设新增 `touchBuzzerDurationMs`，默认 30 ms，可设 20–150 ms。一次有效触摸产生一次短音，拖动过程不连续蜂鸣，关闭写入失败时继续非阻塞重试。该板 CH32 接口将 BEE_EN 暴露为开关，PWM 用于背光，当前没有可验证的蜂鸣音量接口，因此提供时长设置并注明固定音量，不提供虚假的音量滑块。

## 时间同步与本次部署结果

现场只读诊断先确认了根因边界：网络、DNS、网关和 NTP 配置可读，但设备在 UDP/123 无有效响应时一直停在 1970 年；`internetAvailable=true` 只表示链路可用，不能作为时间已同步的判断。固件现在先保持异步 SNTP，连续三次失败后仅在有 PSRAM 的目标上有限调用心知天气同站的 HTTP 时间接口（3 秒超时、最多两次），无 PSRAM 目标不会执行同步 HTTP 阻塞；成功会立即更新时间状态并触发 NTP 同步事件。

2026-09-23 已用 `esp32s3-F16R8` RGB 固件只更新 COM8 对应设备的应用区，文件系统和现场配置未上传。设备重启后 USB 枚举为 COM9，MAC 为 `E8:3D:C1:F7:65:44`，地址 `192.168.5.47`。只读接口返回 `2026-09-23 01:22:26`、`timeValid=true`、`synced=true`；触控屏状态返回 `clock.valid=true`、`quality=good`、页面运行正常。固件 3,680,928 B，SHA-256 为 `c1c904f789653f6b9c8191f705bff93354529560b8b4d1a16327aeb3a156573f`，写入校验通过。证据见 [`postflash-time-check.json`](../artifacts/touch-industrial-20260923/postflash-time-check.json)。

原有生产渲染器像素回归、交互、配置持久化、背光极性、Web 桌面/手机及资源预算检查仍按实施记录保留。Native 2,375/2,375 通过，NTP 回退合同 5/5 通过；实体触摸手感、提示音、亮度光学效果、电机动作和长期运行仍需现场验收，模拟渲染图和主机测试不替代这些实物门槛。
