# 触控屏优化部署记录

用户明确授权“继续部署”后，已完成 COM8 触控设备的固件、Web 资源部署及现场布局适配。设备管理地址为 <http://192.168.5.47>，目标 BASE_MAC 为 `e83dc1f76544`，ESP32-S3 / 16 MB Flash。

## 部署版本与保护范围

- 使用面板专用 `esp32s3-F16R8-rgb-xip40-v12`，保留 RGB / PSRAM XIP 配置；名称保留 xip40，实际 bounce buffer 为 16 行。没有使用普通 S3 构建替代。
- 固件大小 3,592,704 字节，SHA-256：`cf616e14504308959128972ab0e68716d5b79a5eaa9ae04fdeb720f9866b0d60`。
- 写入当前 app1（`0x710000`，分区 `0x700000`）及文件系统（`0xE10000`，分区 `0x1F0000`），写后校验通过。app0、引导程序、分区表、NVS 和 otadata 保留，app0 前后 MD5 一致。
- 替换 57 个 Web 文件；38 个非 Web 文件按现场快照保留，包含已落盘配置及历史文件。未确认刷机前尚在内存中的样本全部落盘，不能承诺保留未落盘数据。
- [部署结果](../artifacts/touch-optimization-20260922/deployment-COM8-r2.json)、[完整日志](../artifacts/touch-optimization-20260922/deployment-COM8-r2.log)、[刷机前文件系统备份](../artifacts/touch-optimization-20260922/deployment-COM8-r2/live-filesystem-before.bin)。备份包含现场配置，按敏感资料保存。

首次专用构建期间其他会话修改了网络依赖补丁，因此该次 sourceStable=false 的产物未部署。本次从已通过本地验收的内容冻结独立目录，387 项验收输入匹配，并验证网络依赖与构建镜像一致后生成最终固件，没有回退共享工作区的其他修改。

证据：[冻结输入核对](../artifacts/touch-optimization-20260922/deploy-source-evidence.json)、[专用构建报告](../artifacts/rgb-resume-touch-optimization-frozen-20260922-build.json)、[Web 归档验证](../artifacts/touch-optimization-20260922/deploy-web-build.json)。此前一次 COM8 瞬时不可用导致连接失败，发生在写入前；后续 r2 部署成功。

## 现场布局适配

迁移保留了原自定义布局。为落实本轮需求，在备份后仅提交屏幕配置，将 revision 11 更新至 12；读取确认 persisted=true、applied=true、restartRequired=false、runtime.running=true。

| 页面 | 已保存内容 |
| --- | --- |
| 首页 | 保留时钟、天气、温湿度和四个页面入口；全局底部提供首页快捷按钮 |
| 趋势 | 保留温湿度最近 10 点来源，扩大两个图表的显示区域 |
| 电机 | 五个无绑定占位替换为单个电机可视化组件，绑定现有 `motor.8.f33733fe`；保留操作确认 |
| 外设执行 | 删除空开关和空场景，保留有效的“采集一次”规则 |
| 系统 | 亮度滑块、自动息屏选择、立即息屏按钮、提示音和重启 |

亮度仍为 70%，自动息屏仍为关闭；保留原默认首页、页面 ID、采集来源与有效规则。电机卡初始设定速度为 50、脉冲为 1600，仅为组件配置，未发送给电机。没有发送正转、反转、停止或电机参数命令。

证据：[适配前配置](../artifacts/touch-optimization-20260922/layout-before.json)、[候选配置](../artifacts/touch-optimization-20260922/layout-candidate.json)、[保存后配置与运行状态](../artifacts/touch-optimization-20260922/layout-after.json)。

## 已验证与剩余边界

- 固件和文件系统写后校验通过，设备重新联网，身份匹配目标设备。
- 实机 API 健康、屏幕状态、屏幕配置和天气状态接口可读；屏幕服务运行且配置已应用，温湿度数据有效，两个趋势均有 10 点历史。见 [API 检查](../artifacts/touch-optimization-20260922/postdeploy-api.json)。
- 约 25 秒串口观察未见崩溃；日志中的 RGB 扫描统计约 42 Hz，采集规则持续运行。该短时观察不代表长期稳定性验收。见 [串口记录](../artifacts/touch-optimization-20260922/postdeploy-serial.log)。
- 首轮实机浏览器确认设备屏幕菜单、首页按钮、心知天气配置、亮度滑块和协议选项卡正常；390 px 页面无横向溢出，无页面 JavaScript 错误。浏览器验收禁止除登录外的写请求。见 [浏览器记录](../artifacts/touch-optimization-20260922/live-browser/README.md)。
- 最终布局复核再次从设备读取 revision 12，配置与保存结果完全一致；首页、电机、系统、外设执行共 4 页分别检查桌面及 390 px 预览，8 张截图已留存，均无横向溢出，HTTP/JavaScript 错误为 0。电机设定与三个方向/停止控件、系统亮度/息屏、首页入口可见，空占位已移除。预览控件保持禁用，实际非只读请求只有登录。见 [最终布局报告](../artifacts/touch-optimization-20260922/live-browser/final-layout-report.json)。
- 最终预览仍如实显示未确认的电机状态；电机/采集卡底部存在 `unknown` 状态文字，手机首页目标页副标题有换行、系统卡小标题较紧凑。这些不属于已验证的实体操作结果，也不宣称视觉效果没有进一步改进空间。
- 天气密钥和城市配置已迁移，但设备时间未校准，状态为 `Waiting for NTP before TLS`。本次未确认心知天气实时请求成功，也未修改网络或时间配置。
- 未执行电机实物动作；电机来源可绑定不等于电机通信或实际运动反馈已验证。触摸手感、触摸唤醒、实体亮度变化、实屏外观及长期运行仍需现场验收。Web 预览截图不是实屏照片。

本地九目标固件、Host、Native、Web 验收证据保留在[实施记录](touch-screen-optimization-implementation.md)，本文件记录后续授权的实际部署，不能将两类证据互相替代。
