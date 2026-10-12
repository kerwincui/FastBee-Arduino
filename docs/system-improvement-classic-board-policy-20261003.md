# G01 经典 ESP32 模组与载板策略

2026-10-03 后继源码增量；不属于已冻结的 r18。新增回归尚待集中执行，不代表已部署或实物通过。

原先所有经典 ESP32 均选择 PZ/WROOM，包含启用 PSRAM 的 F8R8，因此错误地允许 GPIO16/17 作为普通输出。新增 `esp32-wroom`（编号4）、`esp32-wrover`（编号5）模组策略，保留旧编号0～3。生产预检、板型目录和目标资源比较共用同一策略。

- 默认经典构建选择 WROOM 兼容模组策略；`BOARD_HAS_PSRAM` 或 `FASTBEE_USE_PSRAM=1` 选择 WROVER 兼容内存布线策略，GPIO16/17 为 `hard_forbidden`、风险 `psram`。运行期 PSRAM 初始化失败不能解除保留。
- Flash6～11、未引出脚、输入专用脚和模拟通道沿用模组约束；通用模组补 JTAG12～15 风险。资源数继续表示现有软件规划器可分配预算，不扩成全部硬件单元。
- PZ 是独立的载板策略，须显式编译 `FASTBEE_BOARD_PUZHONG_PZ_ESP32=1` 才成为活动策略；与 PSRAM 同选则编译拒绝。只读比较仍可查看 PZ，不能改变当前编译目标。
- 目录新增 `scope=module-policy`，全部 `hardwareVerified=false`。默认引脚不是已确认接线，其他模组/封装/载板不能由这个兼容策略推断支持。

依据：[WROOM-32 v3.8 引脚定义](https://documentation.espressif.com/esp32-wroom-32_datasheet_en.html)、[WROVER-E/IE v2.4 引脚与模组原理图](https://documentation.espressif.com/esp32-wrover-e_esp32-wrover-ie_datasheet_en.html)。两份资料本轮只读核对；旧 WROVER PDF 链接重定向404，不据其空页面作新证据。

准备验证：实际 C++ 选择函数的默认/两种 PSRAM 标记/显式 PZ/冲突编译，真实规划器输出拒绝，完整流式目录的七种身份和内存引脚。测试未执行前不新增 PASS；完整能力矩阵、物理接线和内存实测仍单列验收。
