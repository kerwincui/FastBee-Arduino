# ESP32 TCP 连接预算配置

## 概述

FastBee 设备端 Web 服务器基于 ESPAsyncWebServer + AsyncTCP，其并发能力受 **lwIP TCP PCB 表** 限制。
各芯片的资源禀赋不同，连接预算需差异化配置，否则会导致页面打不开或 SSE 推送丢失。

连接分配遵循以下原则：

```
TCP_TOTAL = SSE（持久推送）+ HTTP（页面/接口请求）
```

- **SSE**：保留槽位给 `AsyncEventSource`（状态实时推送）
- **HTTP**：剩余槽位供页面加载和 API 调用并发使用
- **预算常量**：统一定义在 `include/core/ResourceProfile.h`
- **SSE 上限**：`SSERouteHandler.h` 中 `MAX_SSE_CLIENTS` 固定数组控制
- **TCP 耗尽阈值**：`WebConfigManager.h` 中 `TCP_CONN_EXHAUSTION_THRESHOLD` 控制软重启触发点

> **注意**：AsyncTCP v3.4.10 已不再读取 `CONFIG_ASYNC_TCP_MAX_CONNECTIONS` 宏，该标志为遗留配置。
> 真正的连接数由 lwIP `MEMP_NUM_TCP_PCB`（默认 16）和业务层阈值共同决定。

## 各芯片硬件参数

| 芯片 | 内核 | 内部 SRAM | PSRAM | WiFi |
|------|------|-----------|-------|------|
| ESP32 | 双核 Xtensa 240MHz | 520 KB | 无 | 802.11 b/g/n |
| ESP32-S3 | 双核 Xtensa 240MHz | 512 KB | 8 MB | 802.11 b/g/n |
| ESP32-C6 | 单核 RISC-V 160MHz | 512 KB | 无 | 802.11 ax (Wi-Fi 6) |
| ESP32-C3 | 单核 RISC-V 160MHz | 400 KB | 无 | 802.11 b/g/n |

## 内存预算计算

lwIP 默认配置（所有 ESP32 变体相同，来自 ESP-IDF sdkconfig）：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `MEMP_NUM_TCP_PCB` | **16** | lwIP TCP PCB 硬上限（所有变体，不可突破） |
| `TCP_SND_BUF` | 5744 B | 每连接发送缓冲 |
| `TCP_WND` | 5760 B | 每连接接收窗口 |
| TCP PCB 结构体 | ~172 B | lwIP 内部控制块 |
| **每连接合计** | **~11.7 KB** | PCB + SND_BUF + WND |

单用户访问场景分析（浏览器单 host 最多 6 并发）：

| 状态 | MQTT | SSE | HTTP | TIME_WAIT | 总 PCB |
|------|------|-----|------|-----------|--------|
| 页面加载峰值 | 1 | 1 | 5-6 | 2-4 | 9-12 |
| 稳态 | 1 | 1 | 0-2 | 1-2 | 3-6 |
| 多标签页(S3) | 1 | 2 | 4-6 | 3-5 | 10-14 |

## 各芯片连接预算

| 芯片 | TCP 预算 | SSE | HTTP | 耗尽阈值 | 状态 |
|------|----------|-----|------|----------|------|
| ESP32 classic | 6 | 1 | 5 | 12 | ✅ 已实施 |
| ESP32-S3 | 8 | 2 | 6 | 14 | ✅ 已实施 |
| ESP32-C6 | 6 | 1 | 5 | 12 | ✅ 已实施 |
| ESP32-C3 | 4 | 1 | 3 | 10 | ✅ 已实施 |

> **耗尽阈值必须 < 16**（lwIP 硬上限），在接近上限前触发恢复，留 2-6 个槽位缓冲。
> 内存紧张的芯片（C3）更早触发，内存充裕的（S3）容忍更多连接。

## 关键文件

| 文件 | 作用 |
|------|------|
| `include/core/ResourceProfile.h` | `TCP_TOTAL_BUDGET` / `TCP_SSE_BUDGET` / `TCP_HTTP_BUDGET` 预算常量（按芯片条件编译） |
| `include/network/handlers/SSERouteHandler.h` | `MAX_SSE_CLIENTS` 固定数组槽位数 |
| `include/network/WebConfigManager.h` | `TCP_CONN_EXHAUSTION_THRESHOLD` 软重启阈值 |
| `src/network/WebConfigManager.cpp` | 每 30s 主动 abort TIME_WAIT 连接 + 耗尽检测与自动恢复 |
| `platformio.ini` | 各 `[xxx_runtime_flags]` 中 `CONFIG_ASYNC_TCP_MAX_CONNECTIONS`（遗留标志，AsyncTCP 3.4.10 不再读取） |

## 治理措施

### TIME_WAIT 连接定期清理

ESPAsyncWebServer 关闭连接后，TCP PCB 进入 `TIME_WAIT` 状态（默认持续 2×MSL ≈ 60s），
期间仍占用槽位。`WebConfigManager` 每 30 秒遍历 lwIP PCB 链表，主动 abort 超时的 TIME_WAIT 连接：

```cpp
// WebConfigManager.cpp — 每 30s 清理一次 TIME_WAIT
if (millis() - lastTimeWaitCleanup > 30000) {
    lastTimeWaitCleanup = millis();
    // abort TIME_WAIT connections to free TCP PCB slots
}
```

### SSE 连接数限制

`SSERouteHandler` 使用固定数组 `_slots[MAX_SSE_CLIENTS]` 跟踪客户端。
ESP32-S3 设 `MAX_SSE_CLIENTS=2`（支持多标签页），其余芯片设 `MAX_SSE_CLIENTS=1`（单 SSE 连接）。
超出槽位时拒绝新连接，防止 SSE 占满 TCP PCB 导致 HTTP 请求无法进入。

### 修改注意事项

- 任何阈值修改必须保证 `TCP_CONN_EXHAUSTION_THRESHOLD < 16`（lwIP PCB 硬上限）
- 修改 `ResourceProfile.h` 中的 `TCP_TOTAL_BUDGET` 后，需同步更新：
  - `SSERouteHandler.h` 中的 `MAX_SSE_CLIENTS`（≤ `TCP_SSE_BUDGET`）
  - `WebConfigManager.h` 中的 `TCP_CONN_EXHAUSTION_THRESHOLD`
  - `platformio.ini` 各 `[xxx_runtime_flags]` 中的 `CONFIG_ASYNC_TCP_MAX_CONNECTIONS`
  - `scripts/validate-build-matrix.js` 中的期望值
- ESP32-C3 RAM 最小（400KB），TCP 不宜超过 4
- `CONFIG_ASYNC_TCP_MAX_CONNECTIONS` 已被 AsyncTCP 3.4.10 废弃，但保留作为配置文档和向后兼容
