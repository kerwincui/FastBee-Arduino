# G04：LCD1602 完整驱动写入与失败闭环

状态：2026-10-01 主目录增量已本地验证，属于 r8 之后的候选；不是完整固件、实物或 G04 整包验收通过。

## 依据与故障

审查完整 `src/peripherals/drivers/LCD1602Driver.cpp`，确认原实现存在：初始化直接写低四位而未对数据半字节产生使能脉冲；后续脉冲只保留背光，丢失 DATA/RS；初始化时 `clear()` / `home()` 被尚未初始化的公开守卫跳过；Wire 缓冲短写、NACK、忙锁或租约丢失不向上报告，`begin()` / `print()` 可误报成功。

协议依据取自 Arduino 官方 [LiquidCrystal 1.0.7 发布包](https://downloads.arduino.cc/libraries/github.com/arduino-libraries/LiquidCrystal-1.0.7.zip) 内 `src/LiquidCrystal.cpp` 的 begin/send/write4bits/pulseEnable/clear/home。2026-10-01 实际取得并核对的 ZIP 为 19,947 B，SHA256 `f7b14c42afbbdcfbe66073e6ef1e4cbaa03f5d11f52f9ab91c916b6ccac38434`；该源码 SHA256 `2913d012d4c7398324d690c7d94badfa3242eee829c060adcf8757e90c9d4834`。没有声称获取了尚未取得的芯片原厂 PDF。搜索网关 404 及 GitHub 连接失败仅是资料获取问题；通过官方已发布包完成了本项核对。

## 本轮实现

- 保留项目既有 PCF8574 标准映射：P0=RS、P1=RW、P2=E、P3=背光、P4～P7=DATA。先将 RS/RW/E 置低，等待 50ms，按 `3,3,3,2` 半字节初始化；每个半字节都通过 E 的低/高/低输出，DATA/RS/背光不变。原 5ms/5ms/1ms 等待不缩短。非此映射的背包仍需另行适配，不能仅凭 ACK 判定兼容。
- 初始化命令内部直接发送清屏和归位，并分别等待 2ms；全部 ACK 后才标记初始化成功。初始化失败立即退出并回收自己的总线租约，不自动重试未知的半字节。
- Wire.write 必须返回 1、endTransmission 必须返回 0；即使缓冲短写也结束已开启的事务。任何不确定传输使驱动进入未初始化/失败状态，后续普通写入被拒绝，显式 begin 才重新同步。既有 void 命令签名保留，失败反映在 isInitialized；bool 打印方法逐级传播失败。
- 命令/数据的两个半字节共享 I2C 事务锁，初始化、行更新、自定义字符及清屏等待也受锁保护。任意长度的 print 不持有整段长文本总线锁，而按完整字节串行，避免新增长文本独占。生命周期 begin/end 仍须由现有上层串行；未宣称支持任意并发重配置。
- 初始化/故障标志改为原子布尔值；忙锁失败也保守失效，不让并发中已不确定的传输继续。共享总线忙时 end 仍采用原有 release/deferRelease 路径；不回收别人的租约，不重置共享总线。
- 直接调用驱动也校验现有 PeripheralManager 已规定的 16x2/20x4 和地址范围；避免行数为零/超过偏移表。背光缓存只在 ACK 后更新，空自定义字符指针无写入。浮点打印改为有界 snprintf，拒绝非有限、过大精度和超长结果，防止原 32 字节栈缓冲溢出。

## 本地证据

新增 `test/unit/test_lcd1602_driver.py` 编译完整生产 `.cpp` 和真实 `I2CBusManager.cpp`，仅在 Arduino/Wire/RTOS/日志边界使用主机替身。不是只抽取初始化前半段的源码合同。

| 证据 | 结果 | 边界 |
| --- | --- | --- |
| `artifacts/system-plan-g04-lcd1602-20261001-before-r1.log` | 编译失败保留 | 首次主机夹具未隔离 FeatureFlags 的 ArduinoJson 依赖，不算业务验证 |
| `artifacts/system-plan-g04-lcd1602-20261001-before-r2.log` | 原驱动 14 项中 10 断言失败、2 超时、2 通过 | 原故障证据保留，不覆盖为 PASS |
| `artifacts/system-plan-g04-lcd1602-20261001-local-r1.log` | 15/15 PASS，31.104s | 首批完整驱动与旧退休路径回归 |
| `artifacts/system-plan-g04-lcd1602-20261001-local-r2.log` | 新浮点测试调用二义性导致编译失败 | 按既有 uint8_t 精度签名修夹具；不是放宽业务断言 |
| `artifacts/system-plan-g04-lcd1602-20261001-local-r3.log` | **33/33 PASS，66.329s** | 23 项完整驱动 + 10 项 I2C/显示/传感器准入/观测/样例回归，无跳过 |
| `artifacts/system-plan-g04-lcd1602-20261001-local-r4.log` | **33/33 PASS，65.764s** | 增加测试输入前后哈希核验，断言失败直接退出而不弹 Windows 崩溃窗口；无跳过 |

覆盖初始化全部 43 个传输位置的 NACK 和短写、运行期 5 类非零 Wire 状态与全部字符传输位置、256 个数据字节、完整初始化/清屏/归位与行偏移、背光与自定义字符失败、只允许有效尺寸/地址、检测无设备、失败后重新初始化、共享其它 owner 不被回收、租约丢失、真实线程忙锁/延迟回收及事务互斥。并发失败用真实线程阻塞首笔请求，第二次获取失败后两次都不误报成功，重新 begin 才恢复。

33 项通过之后，290 源文件架构、文件体积与默认测试覆盖登记检查通过；9 个既有体积 warning 未取消。默认 Host unit 自动发现新测试。`platformio.ini` 的 unified_flags 显式启用 LCD1602，后续九环境仍须实际重新编译该增量；不能只看到 FeatureFlags 默认 0 就推断未编译。

最终本地版本 SHA256：驱动 CPP `7db4bcc2ecb9c6a672b29cfa2a5a91bb7975bf432150e9ca9d3cbfdd6eb6b76a`，驱动头 `c6aea807a93b034a3919a52b8634acd252730fc4510d593723cfe6a72f1db6f5`，Python 测试入口 `1677bfc69ccf7162150682779da102c2a9420e08dfce647033f8aa584d07623e`。这是本项版本定位，不替代完整候选摘要。77 文件文档链接、项目卫生、10 文件 UTF-8 和 diff-check 通过。

## 后续门槛

1. 先收取唯一 r8 的真实终态与独立源/镜像/Web/文档归档；r9 另行备份、冻结、完整 Host/Native/九环境验收。本轮改动没有进入活动 r8，不能借用其 PASS。
2. LCD1602 的具体背包映射、电平、电源、实际显示内容、对比度、掉电/断线恢复、与其它 I2C 设备混合负载需实物验证。ACK 只表明扩展器接受总线事务，不证明玻璃屏内容正确。
3. G04 通用质量/展示绑定、可信时间源、SD/UART、完整零代码与其它工作包仍开放；RGB、实物故障/内存实测、24h/7d/V14 未运行或未通过。不访问串口、不刷机、不改现场配置/RTC、不驱动物理输出。
