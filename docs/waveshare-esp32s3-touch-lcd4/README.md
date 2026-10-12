# Waveshare ESP32-S3-Touch-LCD-4 设备屏幕测试配置

配置入口已于 2026-10-07 合并至 [com8-com9-esp32s3-touch](../device-test-configs/com8-com9-esp32s3-touch/README.md)，运行 JSON 仅保存在该设备的 config/ 中。本目录保留硬件说明和历史记录，下文历史默认值及验收结果不能作为当前配置或本轮通过证据。

新版界面使用说明见[设备触摸屏：预览、编辑与操作](../touch-screen-user-guide.md)。该说明对应当前源码；本机部署与实屏验收状态应以本次发布记录为准，下方旧固件版本、IP 和测试记录仅作历史参考。

本机背光极性修正：2026-09-25 用户实物反馈，现场 `backlightActiveHigh=true` 时 100% 黑屏、0% 最亮，确认当前配置方向相反。该 Rev4 显示外设 `panel_display` 应设为 `backlightActiveHigh=false`，公开 `peripherals.json` 已同步。此结论仅针对本机 CH32 背光连接，不全局反转其他屏。逻辑 0% 保留约 10% 可见亮度，100% 最亮；只有显式息屏关闭背光。实际部署后，亮度递增、端点及触摸唤醒效果仍待用户现场复核。

[厂商 CH32 说明](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4/blob/main/examples/esp-idf/ioexpander/README.md)需结合板本和实物判断。生产后端将按配置计算的 duty 直接写入 `0x05`：本机低有效配置对应逻辑 0% 写 229、100% 写 0、息屏写 255。离线测试同时覆盖该板样例与通用两种极性；不能替代部署后的实体确认。通过外设编辑保存极性后需重启设备，不能把保存成功当作显示运行时已重新启动。

[返回配置样例索引](../README.md)

触摸提示音可在“外设配置 → 显示外设 → 触摸提示音时长”选择 20、30、60、100 或 150 ms，默认 30 ms。现有配置缺少 `touchBuzzerDurationMs` 时加载为 30，接口接受 20–150 的整数。开关仍由设备屏幕的触摸提示音设置控制。板载蜂鸣器为固定音量：厂商 CH32 接口通过 EXIO6/BEE_EN 电平开关蜂鸣器，EXIO_PWM 用于背光，未提供蜂鸣器音量寄存器；因此本项调整鸣叫时长，不表示音量可调，也不改外设执行已有的独立蜂鸣动作。

当前操作请使用[新版使用说明](../touch-screen-user-guide.md)；[旧现场快速说明](panel-quick-start.md)仅保留历史记录。

RGB 触控屏的编译期缓冲合同记录在 [`rgb-build-contract.json`](../device-test-configs/com8-com9-esp32s3-touch/rgb-build-contract.json)：运行配置的 `bufferLines=16` 必须对应 `FASTBEE_RGB_BOUNCE_LINES=16`、`FASTBEE_RGB_REQUIRE_BOUNCE=1` 和 30,720 字节双 bounce。`npm run test:contracts` 会始终校验版本库合同；若本机存在候选 ini/构建报告，也会自动交叉校验。发布前可运行 `node scripts/test-rgb-touch-build-contract.js --require-build-report`，防止只改配置却没有用匹配宏重新构建。

目标环境 `esp32s3-F16R8`。触摸设备当前由操作系统枚举为 `COM8`，现场地址为 `192.168.5.33`；历史 `COM9` 只表示此前 USB 重枚举阶段。用户已确认板本 Rev4.0、SHT30；Modbus 步进电机控制器地址为 **8**、波特率为 **9600、8-N-1**，寄存器协议已提供。当前电机硬件断开，示例只准备通用 Modbus 采集任务和禁用控制规则，不试运转、不自动写寄存器；这不是电机专用固件，地址、寄存器及页面都可按其他设备协议重新配置。已移除不再接入的 PM2.5/PM10 任务、规则和页面来源。使用前仍须完成资源预检和硬件匹配检查，不是已通过实物验收的配置。不能替代 COM15 的 ECM50-A 配置，两者虽使用相同构建环境，外设接线完全不同。

## 当前现场状态

当前触摸固件为 r19 `rgb-xip16-r19`。`artifacts/rgb-xip16-r19-build.json` 记录 `sourceStable=true`、`sdkRecompiled=false`、`networkDependenciesMatchCanonical=true`；固件 3,819,712 B，SHA-256 `5d7f322d7dda7628e8cb0f3f24c0e983b3fe5d7c122cfb6a75463e2dee855173`。它继续使用两块各 16 行、合计 30,720 B 的内部 bounce，保留 48 KiB 总空闲和 20 KiB 最大连续块准入；本轮只修正天气启动网络门禁/断网重试，没有修改 SDK、RGB 时序、显示参数、凭据或天气来源配置。

`artifacts/final-deploy-20260920-r7/COM8-r19-write.json` 记录 r19 已写入活动 app1，随后 `artifacts/final-deploy-20260920-r8/COM8-r19-web-continue-write.json` 记录匹配 r8 Web 保留式写入 `written=true`、`verified=true`；恢复 app0 和现场文件系统保护区均未写，写前/写后 MD5 不变，38 个非 `www` 文件和现有配置均保留。蜂鸣器 `controlBuzzerMask/controlInactiveMask=64/0` 来自 r17 的白名单配置调整；r18/r19 firmware-only 更新未再次修改配置。天气状态见下文，实体显示、触摸和蜂鸣器验收仍待收口。

r19 的 120 秒串口短测 `artifacts/r19-COM8-serial-health.json` 显示 STA 已连接 `.33`、NTP 触发、SHT30 连续采集、RGBSCAN 扫描与帧计数连续，未记录 panic 或重启。`artifacts/run20260920-continue-COM8-http.json` 的只读请求为 5/5 HTTP 200，`artifacts/run20260920-continue-COM8-menus/report.json` 的桌面/手机共 24/24 菜单通过且主样式 1,594 条规则可读，`artifacts/run20260920-continue-COM8-live.json` 确认 NTP、SHT30、天气和十点历史均为 `good`；没有手动测试读取或重新设置 Key。

现场后续保存为 revision 9：五页/24 卡，首页合并天气、时钟、SHT30 和页面导航，系统页提供 25/70/100 亮度、息屏、提示音和确认重启；通用模板仍为四页/18 卡，默认不强制独立天气页。仅 `collect_sht30` 规则开启 `allowPanelControl`，并将面板按钮限制为一次只读采集；步进电机和远程控制保持禁用/待绑定。`artifacts/com8-home-system-browser/report.json`、`com8-home-system-menus/report.json`、`com8-home-system-live-final.json` 和 `com8-home-system-binding-tests.log` 分别记录天气/系统预览通过、24/24 菜单、NTP/SHT30/天气/两条十点历史 `good` 和真实 C++ 规则指纹校验（26/26）。亮度动作当前为临时覆盖，设备重启后恢复默认 70；屏幕实体显示、触摸和蜂鸣器仍需收口。

r17 的完整触控 Web 验收未通过：登录、根页和 CSS 传输先后失败；构建日志显示依赖补丁脚本误查 `C:/p12/project/.pio/libdeps/...`，遗漏实际 `C:/p12/libdeps` 中的 AsyncTCP/ESPAsyncWebServer。r18 修正目录解析后，`r18-COM8-panel-browser/report.json` 通过且无配置写入、页面或请求错误；`r18-COM8-web/report.json` 的桌面/手机各 12 个菜单全部通过，零 HTTP、页面与请求错误，登录 5.643 秒、页面最大 2.941 秒。因此 r17 的短时 Web 失败已由 r18 同设备复测闭环，但不能扩展为实体屏或长期稳定通过。

失败窗口同样保留：首份 `r19-COM8-live-data.json` 在启动约 17 秒、SHT30 尚未完成首轮采集时因样本 `unknown` 失败；稍后的 ready 报告才闭环该窗口。`r19-COM8-panel-browser/report.json` 首轮根页出现 `CONTENT_LENGTH_MISMATCH`；复测功能断言通过且识别天气可视化，但截图主框架 CSS 未完整加载，因此不能作为完整样式验收。两次仅写入 r8 Web 的连接失败窗口仍保留为历史，随后已完成成功写入及 24/24 菜单复测。用户仍反馈画面曾黑屏后恢复并出现卷边/漂移，不能把网络、菜单或 RGBSCAN 通过扩展为实体显示/触摸通过。

历史 r16 `rgb-xip16-r16` 使用同样的 16 行 bounce。其构建耗时 325.25 秒，固件 3,817,680 B，SHA-256 `59287cc0da6b041d47d369c130010da6bc9ca0f7593c95335111e9dd29eb4903`；`artifacts/COM8-r16-app1-deploy.json` 的独立完整回读一致。下列 r16 运行和用户观察仅作为升级前基线，不能沿用为当前候选结果。

历史 r16 初始运行报告 `artifacts/rgb-xip16-r16-runtime-initial.json` 为 `passed=true`：普通串口复位后自动启动，本轮无需手动 BOOT 或断电；设备恢复 `192.168.5.33`，三个接口 HTTP 200，耗时 178、181、125 ms。内存保护 `NORMAL`，采集 worker 未暂停，五页 `revision=6` 正在运行，SHT30 为 24.2 C / 60.4 %RH，质量均为 `good`。约两分钟内每 30 秒的 RGBSCAN 扫描数与完成帧数一致，间隔约 23,650～23,661 us；首个窗口内部空闲 41,556 B、最大连续块 31,732 B。顺序只读 Web 短测 9/9 HTTP 200，最大 521.6 ms。用户随后确认 r16 画面短时稳定；这不证明 r17 的画面、点击/滑动、完整浏览器流程或长期稳定。

升级前只读报告 `artifacts/screen-live-state-r16.json` 和 `artifacts/screen-live-data-r17-before.json` 对应 r16 固件，而不是当前运行证据。当时面板配置为 `revision=7`：心知 HTTP 天气源 ready，北京多云 27 °C，`weather.home_weather` 为 `good`；NTP、SHT30 均为 `good`，温湿度趋势各有 10 点。r19 已重新取得北京多云 26 °C、天气/NTP/SHT30/十点历史 `good`。用户提供的测试凭据具体值不写入本目录、公开模板、日志或报告；设备读取接口只返回掩码，r19 没有重复注入或手动测试读取，后续仍验证状态与错误日志不回显密钥。

历史 r15 使用 32 行、61,440 B；冷启动时内部空闲仅 25,524 B、最大块 7,668 B，Wi-Fi `ESP_ERR_NO_MEM`、SHT30 `async_unavailable`，用户看到快速上下抖动和重影，因此运行验收失败。r15 证据继续保留，不能作为 r16 当前状态。

最新主机回归为 Host 662 项中 661 通过、1 项因 Windows 符号链接权限跳过，最终定向 Host 35/35、Native 2,371/2,371、设备屏幕离线浏览器专项 10/10、主样式合同 3/3、部署 helper 57/57。COM15 已部署 canonical r8，COM4/COM16 保持 canonical r7；`.22` S3 r8 与 `.34` C3 r7 的严格浏览器均为桌面/手机共 14 页、主样式 1,594 条规则可读、零失败和零写入。`.33` 触摸设备最新 r8 Web 桌面/手机共 24/24 菜单通过，且 5/5 HTTP 只读检查通过；COM8 独立触摸长稳监测已启动，首轮 12/12 登录、0 次 503/错误，但尚未达到 24 小时，实体屏、触摸和长期运行仍未验收。

当前尚未闭环：实体点击/滑动/方向/边缘与唤醒首触；天气实体画面及另一种自定义 JSON；真实远程开关、场景和“执行一次”回读；Web/采集/历史写入/配置保存/Flash 活动组合负载；24 小时及 72 小时运行。部署文件中的蜂鸣器掩码已经是 `controlBuzzerMask=64`、`controlInactiveMask=0`，但尚未完成有界短响和异常回低电平实测，不能宣称蜂鸣器通过。COM8 首次独立监测因菜单映射遗漏而中断，原始 12/12 登录窗口（0 次 503/错误）仅作诊断证据，不拼接为长稳时长；普通 S3/C3 监测首轮仅完成 1 周期（0 小时），S3 无错误，C3 出现 2 次 503、4 次 HTTP/请求错误和 2 条 console error，自动判定未通过，后续需使用修正版监测重新起算。

## 已核对的硬件信息

| 项目 | 配置依据 |
| --- | --- |
| 主控 | ESP32-S3，16 MB Flash、8 MB PSRAM |
| 显示 | 480×480 RGB LCD；不能使用已有 ST7789/ILI9341 SPI 配置冒充 |
| 触摸 | Touch 型号带 GT911；同系列非 Touch 型号不能假定有触摸 |
| RS485 | 板载自动收发，RX=GPIO43、TX=GPIO44；`pins` 按固件 UART 的 RX/TX 顺序写为 `[43,44]`，不使用 DE GPIO |
| I2C | 主共享总线 SDA=GPIO15、SCL=GPIO7；V1 的 IO 扩展另用 GPIO8/9，不能套用新版复位控制 |
| 控制扩展 | V4 使用 CH32V003，地址 `0x24`；旧版存在 TCA9554 设计，背光/复位方案必须与板本匹配 |

以上依据[微雪产品文档](https://docs.waveshare.net/ESP32-S3-Touch-LCD-4/)、[V4 资料入口](https://docs.waveshare.net/ESP32-S3-Touch-LCD-4/Resources-And-Documents/)、[Arduino 说明](https://docs.waveshare.net/ESP32-S3-Touch-LCD-4/Arduino/)和[官方 V4 仓库](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4)。本次对照的仓库提交为 `aa604c132ef2442013ac93910c65789763329c05`；官方已指出旧版图纸 RS485 引脚表可能与示例方向不一致，Rev4 再次核对为 TX44/RX43。当前配置只适用于已确认的 Rev4，不套用 V1 的扩展引脚，也不把在线产品页仍保留的旧 TCA9554 图表覆盖到 Rev4 的 CH32V003 实物。

## 文件和默认行为

- `device.json`：无设备身份，使用已有 NTP 与时区能力，开发者模式用于测试。
- `network.json`：无 STA 配置、无记忆网络；AP 口令仅为公开工厂默认值，现场部署后更改，不代表已保存任何现场凭据。
- `peripherals.json`：启用共享 I2C、SHT30、RS485 UART 和 Rev4 RGB/GT911 显示。电机通信已确认使用 9600、8-N-1。
- `protocol.json`：主站默认地址及四个 FC03 单寄存器读取任务均为地址 8；任务索引 0～3 分别读取速度、脉冲、计数和波特率编码，全部默认禁用。显式电机寄存器表生成 `modbus:0` 虚拟外设；注册本身不发送命令。MQTT 关闭，无第三方凭据。
- `periph_exec.json`：SHT30 每 10 秒定时采集已启用，并允许通过面板“采集一次”手动执行同一规则；电机四项读取和正转/反转/停止规则均默认禁用，运动规则还保持 `allowPanelControl=false`。只配置 `master.tasks` 不会自动采集，也不会写寄存器。
- `point_history.json`：为温度、湿度、电机速度/计数原始值预留最近十点；没有有效采样不会生成点。
- `screen_panel.json`：绑定 `panel_display`，公开示例有首页、温湿度趋势、外设执行、天气、系统设置五页。首页保留时间、温湿度和天气摘要，页面切换统一由触控底栏处理，卡片空白区域不执行动作；天气源是无 Key 的禁用占位，配置后才请求心知天气。执行页保留已经绑定的“采集一次”。用户可增删、改名、调整顺序和位置，资源上限仍为 8 页、合计 24 张卡片。现场历史 revision 9 的五页/24 卡布局属于旧部署记录，不能视作本次样例或迁移后的实物结果。

Web 菜单“设备屏幕”位于“通信协议”下方，提供“预览 / 配置”两个 Tab。“预览”查看已保存画面和真实缓存数据；“配置”集中管理页面内容、心知天气、显示和息屏，布局编辑按需展开。天气只需启用、填写 Key 和城市，默认 30 分钟更新，不再配置远程 MQTT 通道、预报、鉴权或字段映射。预览按钮不执行设备动作。

旧配置启动时只清理可识别的模板空绑定开关/场景，不按标题批量删除；已经绑定的本地控件、页面 ID、顺序和导航均保留。原始文件只备份一次到 `/config/screen_panel.json.touch-v1.bak`，校验及备份成功后才原子写入新布局；备份异常时继续使用旧配置。完整匹配的三个默认亮度预设合并在原区域内，其他自定义布局不重排。可在 Web 中选择已配置来源添加天气或电机；公开样例不包含天气凭据，也不预填未经设备能力目录确认的电机引用。

系统页提供亮度滑块、自动息屏时间选择、“立即息屏”“提示音”和确认重启。亮度和息屏时间通过后台合并保存，并可在 Web 配置中调整；设备仅支持开关背光时按实际能力显示。息屏后整个首次触摸手势只唤醒，不执行下方控件。蜂鸣器仍为短响，现场 `controlBuzzerMask=64`、`controlInactiveMask=0` 的实体复验状态不因界面更新改变。示例保持 `allowSystemRestart=true` 和重启二次确认；通用默认仍为 false。

“执行一次”是通用动作按钮，可绑定上述允许面板调用的规则，例如手动读取一次已配置传感器或执行场景。它不是周期触发器，不会因打开预览、滑动页面或导入模板而运行。当前“采集一次”绑定 `rule.collect_sht30`，指纹为 `5275bb90`；只授权读取 SHT30，不开放电机或任意远程控制。规则禁用、权限撤销或内容变更后，需重新检查/绑定；返回“已发送”也不代表无回读的远程场景已实际完成。

RGB 配置使用 `st7701_type1`、480×480、硬件旋转 2（180°）；H 前沿/脉宽/后沿为 10/8/50，V 为 10/8/20，与[官方 V4 示例](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4/blob/main/examples/arduino/01_HelloWorld/01_HelloWorld.ino)一致。公开 JSON 的 12 MHz 是[Arduino 参考值](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-4/blob/main/examples/arduino/libraries/GFX_Library_for_Arduino/src/databus/Arduino_ESP32RGBPanel.cpp)。现场 COM8 曾先后使用 6、12 和 16 MHz；按当前 porch 计算，理论帧率约为 21.14/42.27/56.37 Hz。历史上提高到 16 MHz 后左右闪烁更严重；r16 之前的完整固件在 12 MHz 下也曾自动向左移动并横纵卷边。因此不能认定低刷新率是全部原因，也不能把提高 PCLK 当作修复成功。r16 在 12 MHz 下曾由用户确认短时稳定；r19 继续使用 12 MHz，但尚无当前候选实体显示结论，仍须在 Web、采集、历史写入、配置保存和 Flash 活动下复测，并完成 24/72 小时观察。

GT911 不配置 MCU 直连 INT/RST 引脚，由 CH32 控制复位与 INT 同步，运行时轮询触摸。CH32 地址 `0x24`，复位掩码 `0x0A`、GT911 INT 同步掩码 `0x04`、使能掩码 `0x20`，共享 I2C 为 100 kHz。复位期间 INT 拉低，复位释放后继续保持至少 50 ms，再改回输入；该时序用于稳定选择 `0x5D` 地址并完成同步。配置应通过实际资源预检；不能把 JSON 可解析当作引脚冲突检查已通过。

CH32 控制修正遵循[官方驱动](https://raw.githubusercontent.com/waveshareteam/Waveshare-ESP32-components/master/sensor/custom_io_expander_ch32v003/custom_io_expander_ch32v003.c)：方向寄存器 `0x02` 和输出寄存器 `0x03` 使用软件 shadow，不从硬件读回再改写。屏幕独占整个 `controlAddress`，首次从 `0x04` 采样引脚电平并保留未声明位的输出锁存值；只有 active/inactive 掩码明确声明的引脚设为输出，其余为输入，包括未声明的 TP_INT。无法继承前固件未知的方向配置，需要保留控制的输出必须显式声明。TCA9554 后端仍使用可读寄存器的真实读改写，不能混用两者语义。恢复版已部署并有 GT911 按下/抬起日志，但触摸方向和导航仍待实体操作验收。

GT911 配置块还需检查尺寸、触点数和校验；全零或非法配置会拒绝提交，不能仅因校验和为零就判断有效。I2C 能读到产品编号不等于已读取有效配置或触摸坐标，验收须观察实际按下/抬起与页面响应。

Rev4 CH32 的蜂鸣器输出是 bit 6（`0x40`），系统使能是 bit 5（`0x20`）。公开模板目标使用独立 `controlBuzzerMask=64`、`controlInactiveMask=0` 和 `controlTouchIntMask=4`；蜂鸣器位由显示后端共享的 CH32 输出 shadow 控制，不能再建一个独立外设直接写同一个 `0x24` 地址。蜂鸣器默认保持低电平，面板动作只产生有界短响，异常路径必须确保回到低电平。r17 部署文件把设备配置从历史 `0/64` 调整为 `64/0`，并证明除此之外的 JSON 内容相等；r18/r19 firmware-only 更新保留该文件系统。尚未完成有界短响和异常回低电平实测。各控制掩码通用默认值均为 `0`，不把此板规则套用到其他硬件。r19 串口及 ready 报告已取得 SHT30 真值和两条十点历史；当前电机断开，无 Modbus 实测。当前状态见[实施记录](../screen-panel-implementation-report.md)。

后端使用 `esp_lcd` 和一张 460,800 字节的 PSRAM RGB565 帧缓冲，16 行绘制 stripe 另需 15,360 字节并优先分配到 PSRAM，snapshot 和任务栈另计；旧约 1 KiB 的填充行缓冲已移除。通用默认最多请求两块、每块 4 行的内部 DMA bounce，共 7,680 字节；当前 r19 目标按受控构建配置请求两块各 16 行、合计 30,720 B，并执行 48 KiB 总空闲和 20 KiB 最大连续块准入。历史恢复版曾回退为 0，不代表当前值。不引入完整 LVGL/GFX 库。当前 `st7701_type1` 只支持 480×480；背光支持百分比调节，灭背光时 RGB 仍刷新，不表示进入省电睡眠。本例关闭自动休眠，先完成负载显示、方向和触摸验收。

官方 Arduino GFX 示例在 480 像素宽度请求两块各 40 行的内部缓冲，共 76,800 B。FastBee 的 r14 采用该缓冲大小及 XIP/cache-safe SDK 配置，但因启动内存不足回退；r15 改为 32 行、61,440 B，仍挤压 Wi-Fi 和异步采集内存且实体抖动重影。r16 改为 16 行、30,720 B，联网和采集短测恢复、扫描计数连续，用户短时观察画面稳定；r19 保持该缓冲规格并已部署，短时启动、网络和天气数据链通过，但实体显示、完整样式和长期负载仍待验收。

这里仍是基于 pioarduino 提供的官方 Arduino-ESP32 3.3.8 / ESP-IDF 5.5.4 SDK 开发。所谓“重编 SDK”只是让官方源码组件按上述 `sdkconfig` 编译期选项在隔离目录中重新生成，不是修改或私有分叉 SDK，也不会覆盖 PlatformIO 的共享 SDK 安装。隔离 diagnostics `__wrap_log_printf` 条件补丁及 builder 适配只修复当前 PlatformIO/pioarduino 完整重编路径的兼容问题，不改变显示、触摸或板级硬件逻辑；隔离副本和共享包均受摘要/恢复检查保护。

官方指出 PSRAM 访问竞争、Flash 写入期间外部缓存不可用均可能影响 RGB 输出；增加 bounce 只能增加部分短时波动余量，不能单凭缓冲变大就宣布稳定。实测应同时观察静态页面、翻页、Web 访问、真实采集及历史落盘时的画面。[ESP-IDF 5.5.4 RGB 缓冲说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/peripherals/lcd/rgb_lcd.html#bounce-buffer-with-single-psram-frame-buffer)。

默认亮度为 70%，首页始终保留时钟和页面入口；传感器无读数时显示未知，不应导致整个首页空白。运行时长与网络状态绑定固件的系统来源，部署后须核对实时值。所有外设名称按 UTF-8 字节计不超过 48，避免配置文件可解析但外设初始化被名称校验拒绝。

修正版固件在 GT911 初始化失败时保留已启动的只读显示并记录告警，不因触摸缺失关闭显示；触摸未就绪时也不会自动息屏。此降级只能保证继续展示信息，页面导航与控制仍需触摸恢复后验收。当前模板已显式关闭自动休眠，因此不会用超时息屏掩盖显示问题。

现场历史上曾先后反馈黑屏、16 MHz 下闪烁加重、左右/上下卷边以及 r15 的快速上下抖动重影；r16 恢复联网和采集后，用户曾短时观察确认画面稳定。该 r16 观察不能扩展为 r17 的显示、触摸、完整负载或长期通过。恢复修正针对 SEVERE 内存保护暂停屏幕后的恢复：必须真正重启显示运行时成功，才清除内存暂停状态；失败保留待重试，不会在 Web 中假报恢复。恢复窗口内同一屏幕的修改、禁用或删除返回 `SCREEN_MEMORY_RESTORE_IN_PROGRESS`，稍后重试。历史恢复版 `ac130e…` 的日志确认 SEVERE 后真正恢复显示运行时、GT911 配置有效并出现按下/抬起，恢复时 bounce 退回 0 字节。它只证明当时运行时和触摸输入链路有进展，不代表触摸方向或导航已通过。现场另禁用 mDNS 以节省内部 RAM，不改变公开模板的通用默认规则。

## Modbus 步进电机接入

现场已确认地址 8、9600、8-N-1，用户协议中的地址 `01` 只是样例；结构化 Modbus 调用按实际地址 8 生成 RTU 帧和 CRC，不复制样例 CRC。以下十六进制地址在 JSON 中写十进制数；`actionValue` 也使用十进制 `地址:数值`。

| 操作 | 功能码/寄存器 | 协议值及示例处理 |
| --- | --- | --- |
| 正转/反转/停止 | FC06，`0x0000` / `0x0001` / `0x0002` | 写 1；三条 `actionType=17` 规则分别为 `0:1` / `1:1` / `2:1`，全部禁用、未授权 |
| 速度 | FC03 单读 / FC06 写，`0x0005` | 用户写示例 `0x00FF=255`；本包只建读取任务，不自动设置速度 |
| 脉冲 | FC03 单读 / FC06 写，`0x0007` | 写示例 255；本包只读 |
| 计数 | FC03 单读 / FC06 写，`0x0004` | 写示例 255；本包只读，不假定等于机械绝对位置 |
| 波特率编码 | FC03 单读 / FC06 写，`0x000A` | 1/2/3/4/5 分别为 9600/19200/38400/57600/115200；本包只读编码，不建写规则 |
| 设备号 | FC06，`0x0006` | 协议已记录，不建写规则，不改变现场地址 |

四个读取任务分别以 FC03、`quantity=1` 读 5、7、4、10。`collect_motor` 使用四个独立 `actionType=18` 动作，各自只含一个 `poll` 任务索引 0、1、2、3 及空 `ctrl`，避免批量轮询自动合并相邻寄存器而扩大读取范围；不要改成一个多任务批次。当前任务与规则都禁用，接回硬件后再启用只读采集。速度/计数暂按 uint16、倍率 1 显示原始值，未确认的物理单位不猜测成 rpm、角度或距离。

`type=51` Modbus 虚拟外设由通信协议生成，不能直接塞入 `peripherals.json`。示例显式填写 `motorRegs=[0,1,2,5,7]`、地址寄存器 6、波特率寄存器 10，用于生成 `modbus:0` 并供通用 FC06 写动作引用，不依赖隐式 YF-53 表。当前共享电机服务的 `readStatus` 分别单读速度和脉冲寄存器，不读取中间地址，也不据此判断运动方向；计数/波特率仍由本例四条通用采集任务读取。软限位最小/最大值均为 0，保持关闭，避免产生额外的脉冲预写；这也意味着本例没有已验证的行程保护。`modbusRtu.slaveAddress`、任务和虚拟设备条目均明确设置为地址 8。

默认屏幕不放置电机卡。三条控制规则均 `enabled=false`、`allowPanelControl=false`；需要控制时先接回硬件并完成只读核对，再由用户启用明确规则、允许面板调用，并通过设备屏幕专用 API 保存绑定和指纹。正反转建议二次确认；未绑定的停止按钮不能当作紧急停止手段。示例规则使用平台触发类型且 MQTT 默认关闭，没有启动/周期触发，不能将启用规则理解成已运行。动作写入应答也不等于运动已完成。

通用数值控件已经可以绑定受限 FC06 规则，但本公开包不会在电机断开时默认开放。完成只读核对后，可另建 `useReceivedValue=true`、`actionValue="5:$value"` 的速度规则，设置明确的最小值、最大值、步长和整数精度，再显式启用 `allowPanelControl`；设备屏幕只显示加减与执行，不接收任意寄存器地址或 RAW 帧。步进电机专用动作本身不在数值白名单中，方向/停止继续使用独立固定动作并建议确认。寄存器 5 的实际速度单位和安全上限尚未提供，不能直接把 0～65535 协议范围当作安全机械范围。

硬件断开且从未采样时，Web 专用保存接口会拒绝尚不存在于缓存的数值来源；当前默认布局因此不预置电机数据卡。硬件接回并成功只读采集后，再从来源列表添加速度、脉冲或计数卡。配置文件中的任务和来源名称不代表已经取得实物读数。

## I2C 温湿度接入清单

已采用现有 `type=38` 通用传感器、`driver=SHT30`，通过 `busPeripheralId=panel_i2c` 复用 SDA15/SCL7 的 100 kHz 总线。模板地址为常用 `0x44`（十进制 68），需要在实机确认；地址跳线也可能使其为 `0x45`（69）。总线扫描只确认应答，不能替代型号识别，不要向 CH32、GT911、RTC 等地址发送试探性的写命令。

用户给出的“14 bit、4.7 kΩ 上拉、0.1 滤波”不直接转换成不存在的固件字段。当前 SHT3x 驱动使用 `0x2400` 单次测量命令，读取两个 16 位原始值并检查 CRC，未提供任意分辨率配置。4.7 kΩ 是硬件上拉，不是软件开关；需结合板载上拉检查并联阻值。“0.1 滤波”含义尚待确认，本包仅把展示精度设为一位小数，不将其误写为滤波系数或量程比例。

模板已按测试目标启用相关外设和规则；部署前确认资源预检，部署后确认 `i2cSensors` 和 `screenPanel` 实际激活及传感器地址。温湿度动作只选择 `temperature` / `humidity` 驱动通道，缓存源分别为 `sht30_temperature` / `sht30_humidity`；`air_temperature` / `air_humidity` 只是可选上报字段，不替代缓存源。

使用采集规则获得真实温湿度数据后，再从面板来源列表选择温度/湿度。最近十点功能需启用历史保存并按需配置来源；无读数时显示未知，不用固定样例值冒充实测。

## 预检与部署

仅预检，不写设备：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/deploy-config-bundle.ps1 `
  -Env esp32s3-F16R8 -Port COM8 `
  -BundlePath docs/waveshare-esp32s3-touch-lcd4 -Features screenPanel,i2cSensors -DryRun
```

实际部署先完成设备身份和 Flash 备份检查，再在本地私有副本填写联网与已确认的外设参数。STA 凭据通过现有部署环境变量提供，不写回公开配置。首次验证按“启动/联网 → Web/API → I2C/Modbus 真实采集 → 屏幕显示/触摸”依次进行；每层分别记录结果，不把预检或构建通过当作实物通过。电机当前断开，所有读取和运动规则保持禁用，不发送任何正转、反转、停止、速度、脉冲、计数、地址或波特率写命令。

COM8 与 COM15 是两台同构建环境设备：不要仅按 `esp32s3-F16R8` 选取或覆盖私有备份，必须同时核对串口和芯片身份。通用整包烧录会替换配置，本次保留现场配置的更新应使用已验证的专用流程；不要为了复用样例直接覆盖另一台 S3 的文件系统。
