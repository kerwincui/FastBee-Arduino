---
title: TCP连接预算
order: 77
---

# ESP32 TCP 连接预算

> lwIP TCP PCB 池管理策略，防止页面打不开或 SSE 推送丢失。

## 背景

ESP32 的 lwIP TCP PCB 硬上限为 **16**（所有变体相同）。连接分配遵循：

```
TCP_TOTAL = SSE（持久推送）+ HTTP（页面/接口请求）
```

## 各芯片连接预算

| 芯片 | TCP 预算 | SSE | HTTP | 耗尽阈值 | 
|------|----------|-----|------|----------|
| ESP32 | 6 | 1 | 5 | 12 |
| ESP32-S3 | 8 | 2 | 6 | 14 |
| ESP32-C6 | 6 | 1 | 5 | 12 |
| ESP32-C3 | 4 | 1 | 3 | 10 |

> **耗尽阈值必须 < 16**（lwIP 硬上限），留 2-6 个槽位缓冲。

## 单用户场景分析

| 状态 | MQTT | SSE | HTTP | TIME_WAIT | 总 PCB |
|------|------|-----|------|-----------|--------|
| 页面加载峰值 | 1 | 1 | 5-6 | 2-4 | 9-12 |
| 稳态 | 1 | 1 | 0-2 | 1-2 | 3-6 |
| 多标签页(S3) | 1 | 2 | 4-6 | 3-5 | 10-14 |

## 治理措施

### TIME_WAIT 定期清理

`WebConfigManager` 每 30 秒遍历 lwIP PCB 链表，主动 abort 超时的 TIME_WAIT 连接。

### SSE 连接数限制

- ESP32-S3：`MAX_SSE_CLIENTS=2`（支持多标签页）
- 其余芯片：`MAX_SSE_CLIENTS=1`（单 SSE 连接）
- 超出槽位时拒绝新连接

## 关键文件

| 文件 | 作用 |
|------|------|
| `include/core/ResourceProfile.h` | `TCP_TOTAL_BUDGET` / `TCP_SSE_BUDGET` 常量 |
| `include/network/handlers/SSERouteHandler.h` | `MAX_SSE_CLIENTS` 槽位数 |
| `include/network/WebConfigManager.h` | `TCP_CONN_EXHAUSTION_THRESHOLD` |
| `src/network/WebConfigManager.cpp` | TIME_WAIT 清理实现 |

> 完整说明请参考项目仓库 [`docs/tcp-connection-budget.md`](https://github.com/kerwincui/FastBee-Arduino/blob/master/docs/tcp-connection-budget.md)。
