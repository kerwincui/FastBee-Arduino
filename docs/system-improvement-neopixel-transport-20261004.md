# NeoPixel checked transport 接续（2026-10-04）

状态：主目录后继已完成2026-10-04集成定向r3的相关Host回归（整组284项PASS），包含11个普通来源/真实帧消费者场景和transport故障场景。不是r19冻结内容或完整目标验证，未改动隔离和安装SDK。最新r5实际S3/C6编译因仲裁源缺锁头FAIL，已补但尚待重编；实物项全部NOT_RUN。

普通控制已接现有来源、配置/规则版本、operation与hold策略；动画一次调用只发送一帧。brightness只修改运行配置、attempted=false；普通stop撤销旧操作后续帧且不发送黑帧、不释放对象。下面的黑帧/释放说明特指生命周期清理，不是普通停止。实际公开规则路径为CALL_PERIPHERAL的action/value对象，脚本PERIPH COLOR及MQTT未匹配规则的直接颜色入口不受支持；外部NeoPixel文档已同步修正。

## 证据与修复范围

仓库固定 Adafruit NeoPixel 1.15.5、Arduino ESP32 3.3.8。只读核对本机实际源码与官方对应 tag 后，确认 IDF5 分支三类问题：`rmtInit` 失败直接返回且未释放 `show_mutex`；`rmtWrite` 使用无限等待并忽略失败；零长度释放没有检查目标 pin、`rmtDeinit` 结果或遗留缓冲状态。

`scripts/apply-libdeps-patches.py` 现为 NeoPixel 检查版本及三个源码 SHA256，拒绝未知版本、同版本漂移和修改过的补丁。仅替换 IDF5 分支，保留许可证和完整 legacy 分支。补丁 payload 与原始源码都登记在 `patches/libdeps/Adafruit_NeoPixel@1.15.5`；正常构建钩子才会应用它。此批没有直接覆盖 `.pio/libdeps`。

新 C ABI 返回明确结果，项目不再从 `void show()`、`new` 或 `begin()` 推断 RMT 成功。设置、单色、8 种动画及 rainbow/cycle/next 均传播失败；只有成功的一帧才推进索引。初始化失败不再写“initialized”。快照仍是 `service_lifecycle/allocated`，不是 RMT 健康或实物灯珠计数；故障后 `cleanup_pending` 也不会伪造 initialized 数量。

r3 新增独立 `NeoPixelResourceSnapshot` 与 PM 零等待方法，发布本项目唯一 Adafruit 对象的真实 ID 副本、pin、配置 LED buffer 数量、对象存在/生命周期、最近 write/release 的 attempted/result，以及项目退役保留和 backend quarantine。没有 PM 同步锁或锁忙时只返回未观测，不能当空表；成功清理才清账，配置 map 回滚不抹除原 ID。查询不构造 driver、不解引用 SDK 对象、不调用传输 ABI 或 SDK。backend 保留标志在实际 write/release 后登记，来源 `fastbeeNeoPixelPinRetained` 在固定补丁 `esp-idf5.c:26` 仅执行一次 acquire 原子加载；该记录不是对直接绕开 PM 的第三方传输的全局原子快照。capacity=1 是项目单例限制，不是 RMT channel 数量；`ledCount` 不证明实际灯珠数量。

## 超时、内存与退役

| 情形 | 行为 | 后续 |
| --- | --- | --- |
| 业务发送中 mutex 分配失败/获取繁忙、发送前符号分配失败 | 返回失败；不宣称发送成功 | 可在业务路径重试；保留既有 buffer |
| RMT 初始化失败、发送拒绝或超时 | 释放应用 show mutex，隔离 backend；保留 pin、buffer 和 Adafruit 对象 | 等排障、确认后重启；晚到完成不自动解除隔离 |
| deinit 失败、检测到 periman owner 改变 | 不忘记旧 pin，不删除对象，不尝试再次释放 SDK 资源 | `cleanup_pending`，需重启/复核 |
| 正常退役 | 用现有对象发送关闭帧，确认完成及释放后才析构 | 未创建的灯带不会因 teardown 新建对象 |
| 任意 teardown 失败（也包括发送前 OOM/繁忙） | 独立的项目级原子 pin 保留；add/update/enable 失败保留错误配置，删除/持久化回滚不得丢失对象 | `cleanup_pending`；不重试旧对象输出，排障后受控重启 |
| 配置 map 后续被替换或移除 | SDK 及项目级原子 pin 标识独立于配置 map | pin 冲突与 setup 路径仍拒绝复用隔离 pin |

符号缓冲分配到内部 8-bit RAM；RGB 单颗至少 3 B 像素 + 24×4 B 符号，估算为 `99×N + 512 B`，512 B 仅是静态余量，不是完整运行峰值测量。当前默认 `FASTBEE_NEOPIXEL_MAX_LEDS=64`，定制构建的上限、碎片化与 RMT channel 竞争仍须单独核验。原有固定 2 KB RMT 估算已改正。

show mutex 等待 50 ms，提交前保留 ≥300 μs latch 间隔，传输事件等待 100 ms。SDK 3.3.8 实际把 timeout 参数直接作为 RTOS ticks，补丁显式转换并在编译时拒绝未审阅的 SDK 版本。SDK 内部锁、初始化和析构不属于已证明的硬实时上界，因此不宣称整个调用最多 100/150 ms。periman 指针核对只是一致性检查，不是跨第三方并发 GPIO 改配的 generation lock。

## 集中验证入口

r20 预检在 S3/C6 的 NeoPixel dependency `esp.c:40` 真实失败：`peripherals/NeoPixelTransport.h: No such file or directory`。原失败归档保留。根因是 PlatformIO dependency builder 克隆 pre-build environment，而项目 include 目录仅由项目 builder 追加，旧 Host 夹具手动传入 `-I include` 掩盖了该差异。后继候选在两个 SCons hook 路径通过 `PrependUnique(CPPPATH=...)` 注册规范头文件目录，并核对规范 C ABI 的 LF 归一化 SHA256，拒绝缺失或漂移；不复制 ABI、不直接修改已安装依赖。回归从实际 hook 获取编译路径，准备空格路径、幂等与缺失/漂移断言。修复尚未运行，等待 r2 集中验证，不追溯改写 r20 FAIL。

r2 Host 中 patch 7 项与真实 transport 8 项通过；PM 集成夹具缺少新 `managedPinConflict` 依赖导致 setUpClass ERROR。r2 整批 FAIL，S3/C6 另在 ADC snapshot aggregate 赋值编译失败；未获得固件完整通过。r3 已补夹具依赖并加入真实拒绝分支回归，另准备上述 NeoPixel ledger 的真实身份/发送失败/释放失败/零等待忙锁/冷态与清空回归；这些新测试尚未运行，原错误证据保留。

- `test/unit/test_neopixel_transport.py`：真实生成补丁源码 + SDK doubles 验证 init 失败不锁死、有限等待、超时/晚完成保留 buffer、wrong pin/owner、deinit 失败、分配上界/OOM、并发首次 mutex；patch 版本/摘要/幂等/写失败恢复；真实控制与删除函数、NeoPixel 初始化/退役分支、add 失败回滚尾部、update stop 与持久化回滚分支的执行断言。
- `test/unit/test_feature_additional_owners.py`：真实 owner 生命周期，覆盖 begin/pixel allocation 失败、输出错误、退役失败、允许重试的错误与纯被动并发查询。
- `scripts/validate-libdeps-patches.js`：补丁资产及原始摘要。已安装 NeoPixel 源码由版本/摘要 build hook 检查，checked ABI 缺失会导致构建链接失败。
- 上述随后进入完整 Host/Native/九环境门禁；本文件不填入尚未运行的 PASS。所有集中运行必须沿用 `scripts/firmware-build-lock.ps1`、登记的 D: 构建缓存和独立证据目录。

还需要真实目标的有限等待/高负载测试、与 DHT/IR 等 RMT 竞争、内存峰值与碎片化、断开/错误接线及 24h/7d 稳定性证据。灯带没有反馈通道，SDK 发送完成也不证明颜色、数量、电平、实际熄灭或供电安全。设备阶段维持无串口/无刷机/无物理输出，授权与接线安全确认后才能开展。

外部设备文档同步到 `D:/project/gitlab/FastBee-doc/src/device/peripherals/neopixel.md` 和 `examples/14-rgb-neopixel.md`，明确默认数量上限、故障保留与重启语义。
