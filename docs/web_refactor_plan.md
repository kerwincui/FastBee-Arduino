# Web 精简版与稳定性说明

本文档记录当前 Web 精简版的功能范围、构建策略和稳定性优化。它替代早期“待办计划”式描述，作为当前代码状态说明使用。

## 当前目标

默认精简版面向 classic ESP32、ESP32-C3、ESP32-S3，目标是让本地 Web 管理界面在低内存、低并发资源下保持可访问、可操作、可长期运行。

精简版保留核心业务闭环：

- 登录和会话保持
- 设备监控首页
- 网络配置
- 设备基础配置、NTP、缓存管理
- 配置按类型导入/导出
- 外设配置
- 外设执行规则
- MQTT 配置
- Modbus RTU 主站、子设备、映射和控制
- 设备控制页面

精简版默认移除或不发布以下高资源/低频功能：

- OTA 页面与 OTA 路由
- 文件管理
- 日志查看
- 多用户和角色管理
- RuleScript 页面与脚本引擎
- TCP / HTTP / CoAP 协议页面与处理器
- 独立全屏监控页
- Modbus 从站
- BLE、LED 灯带等默认关闭能力

ESP32-S3 可使用 `esp32s3-full` 构建完整功能验证版本。

## Web 源码与产物

| 路径 | 用途 |
| --- | --- |
| `web-src/` | 可维护的 Web 源码，包含页面、CSS、JS 模块和 i18n |
| `data/www/` | 上传到硬件的 gzip 压缩产物，不建议手工编辑 |
| `.pio/fs-staging/run-*/www` | 构建脚本生成的干净 LittleFS staging 目录 |
| `scripts/build-web-modules.js` | 从 `web-src` 生成发布模块 |
| `scripts/gzip-www.js` | 构建、压缩、生成 staging，并可上传 LittleFS |

当前精简构建会清理历史遗留产物，避免 `data/www` 中留下已经不属于精简版的旧页面或旧模块。

## 加载策略

Web 首屏按核心 chunk 串行加载，降低 ESP32 lwIP socket 并发压力。中文翻译分片延后加载，避免登录后第一屏被大 i18n 文件阻塞。

页面采用按需加载：

- 首页登录后优先加载设备监控数据。
- 常用页面片段在后台低速预加载。
- 协议页面默认先加载 MQTT 轻量配置。
- Modbus RTU 详细配置和控制模块按需加载。
- 设备控制页面拆分为核心、视图、Modbus 控制等模块。
- 外设执行表单和 Modbus 子设备相关逻辑拆分加载。

## 请求治理

前端请求通过 `request-governor.js` 分级：

| 等级 | 用途 |
| --- | --- |
| `cheap` | 状态、小配置、可缓存接口 |
| `normal` | 页面常规数据、列表 |
| `heavy` | 协议配置、Modbus 控制、配置导入等高成本请求 |

批量请求缓存会保留完整 `{ success, data }` 响应，避免刷新后只命中 `data` 导致页面不渲染。配置分片导入使用 `heavy` 请求档位，防止和页面刷新请求互相挤占。

## 配置导入导出

设备配置的高级配置页面支持按类型导入/导出 `/config/*.json`。导入统一走 `/api/config/transfer/import-chunk` 分片接口，每片约 `6KB`，避免大文件内联上传触发 413。

详见 [配置导入导出指南](./config-transfer-guide.md)。

## 当前体积状态

最近一次精简构建结果：

- Web 压缩文件数：约 36 个
- Web gzip 总量：约 176KB
- `data/www` 总量：约 182KB
- staging 数据目录总量：约 208KB

体积会随页面文案、配置和模块拆分略有波动。判断部署体积时以 `node scripts/gzip-www.js --web-slim --no-upload --no-monitor` 输出为准。

## 后续可继续优化的方向

1. 按硬件场景关闭不用的能力，例如不用 OLED/数码管时关闭 `FASTBEE_ENABLE_LCD` 或 `FASTBEE_ENABLE_SEVEN_SEGMENT`。
2. 继续清理 `main.css` 中完整版才需要的样式。
3. 将外设执行表单中低频编辑器逻辑进一步按需拆分。
4. 对 Modbus RTU 控制面板继续降低首次打开时的模板体积。
5. 将文档截图与当前精简版菜单保持同步。

稳定性优先级高于页面功能堆叠。默认精简版应优先保证核心页面可打开、按钮可响应、配置可保存、规则可运行。
